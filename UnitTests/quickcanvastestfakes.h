// MIT License
#ifndef QUICKCANVASTESTFAKES_H
#define QUICKCANVASTESTFAKES_H

#include "hittestsource.h"
#include "jobsubmitter.h"
#include "pagesurfacerenderer.h"
#include "viewportcontroller.h"

#include "pdfprocessingbudget.h"

#include <QColor>
#include <QHash>
#include <QImage>

#include <memory>

namespace
{

constexpr QSizeF PageSizeMM = QSizeF(100.0, 100.0);
constexpr qreal PixelPerMM = 2.0;
constexpr qreal PageBoxSize = 100.0;

pdf::PDFRevisionIdentity makeRevision(const QString& documentId,
                                      quint64 documentRevision = 1)
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = documentId;
    revision.document.sourceDataHash = documentId.toUtf8();
    revision.documentRevision = documentRevision;
    return revision;
}

class FakeGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    explicit FakeGeometrySource(int pageCount) :
        m_pageCount(pageCount)
    {
    }

    int pageCount() const override { return m_pageCount; }

    QSizeF pageSizeMM(int pageIndex,
                      pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        const bool transposed = extraRotation == pdf::PageRotation::Rotate90 ||
                                extraRotation == pdf::PageRotation::Rotate270;
        return transposed ? PageSizeMM.transposed() : PageSizeMM;
    }

    QTransform pagePointToDeviceMatrix(int pageIndex,
                                       const QRectF& deviceRect,
                                       pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);
        QTransform matrix;
        matrix.translate(deviceRect.left(), deviceRect.bottom());
        matrix.scale(deviceRect.width() / PageBoxSize,
                     -deviceRect.height() / PageBoxSize);
        return matrix;
    }

private:
    int m_pageCount = 0;
};

class FakeRevisionSource final : public pdfinteraction::IDocumentRevisionSource
{
public:
    pdf::PDFRevisionIdentity currentRevision() const override { return revision; }
    bool isCurrent(const pdf::PDFRevisionIdentity& candidate) const override
    {
        return candidate == revision;
    }

    pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));
};

class InlineJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId = spec.jobId.isEmpty()
                                  ? QStringLiteral("job-%1").arg(++m_sequence)
                                  : spec.jobId;
        auto token = std::make_shared<pdf::PDFJobCancellationToken>();
        pdf::PDFJobContext context(token,
                                   pdf::PDFProcessingLimits::conservativeDefaults(),
                                   [](int) {});
        work(context);
        return jobId;
    }

    bool cancel(const QString& jobId) override
    {
        cancelledJobs.append(jobId);
        return false;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = pdf::PDFJobStatus::Succeeded;
        return result;
    }

    void publishCurrentRevision(const QString& documentKey,
                                const pdf::PDFRevisionIdentity& revision) override
    {
        publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override
    {
        publishedRevisions.remove(documentKey);
    }

    QStringList cancelledJobs;
    QHash<QString, pdf::PDFRevisionIdentity> publishedRevisions;

private:
    quint64 m_sequence = 0;
};

class FakePageSurfaceRenderer final : public pdfinteraction::IPageSurfaceRenderer
{
public:
    pdfinteraction::PageSurfaceResult render(
        const pdfinteraction::PageSurfaceRequest& request,
        pdf::PDFJobContext& jobContext) override
    {
        ++renderCount;
        renderedKeys.append(request.key);

        pdfinteraction::PageSurfaceResult result;
        result.key = request.key;
        result.token = request.token;
        if (jobContext.isCancellationRequested())
        {
            result.state = pdfinteraction::SurfaceTerminalState::Cancelled;
            result.typedError = QStringLiteral("page-surface/cancelled");
            return result;
        }

        QImage image(request.key.targetPixelSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(fill);
        result.state = pdfinteraction::SurfaceTerminalState::Complete;
        result.pixels = pdfinteraction::makeSurfaceBuffer(std::move(image));
        result.pixelSize = request.key.targetPixelSize;
        result.byteSize = result.pixels ? result.pixels->byteSize : 0;
        return result;
    }

    void shedPrefetchAndQuality() override { ++shedCount; }

    int renderCount = 0;
    int shedCount = 0;
    QColor fill = QColor(Qt::white);
    QList<pdfinteraction::PageSurfaceKey> renderedKeys;
};

}   // namespace

#endif   // QUICKCANVASTESTFAKES_H
