// MIT License
#include "quickoutput.h"
#include "pdfartifactidentity.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsession.h"
#include "pdfoptionalcontent.h"
#include "pdfpage.h"
#include "pdftransparencyrenderer.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImageWriter>
#include <QJsonArray>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <cmath>

namespace loopeditor
{
namespace
{
class OutputRenderer final : public pdf::PDFTransparencyRenderer
{
public:
    using pdf::PDFPageContentProcessor::getRenderErrors;
    using pdf::PDFTransparencyRenderer::PDFTransparencyRenderer;
};
}

void prepareOutput(const OutputRequest& request, OutputResult& result, pdf::PDFJobContext& job)
{
    if (!request.document || request.revision.isEmpty() || request.dpi < 72 || request.dpi > 600)
    {
        result.error = QStringLiteral("output/invalid-request");
        return;
    }
    result.staging = std::make_shared<QTemporaryDir>();
    result.stagedPath = result.staging->filePath(QStringLiteral("output"));
    if (!result.staging->isValid())
    {
        result.error = QStringLiteral("output/staging-unavailable");
        return;
    }
    const auto hash = [](const QByteArray& bytes)
    { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    result.record = QJsonObject{
        { QStringLiteral("revision"), request.revision },
        { QStringLiteral("printProof"), false },
        { QStringLiteral("fidelityAcknowledged"), request.fidelityAcknowledged }
    };
    if (job.isCancellationRequested())
        return;

    if (request.format == OutputFormat::PublishedPdf)
    {
        QByteArray bytes = request.publishedBytes;
        if (bytes.isEmpty())
        {
            QFile source(request.sourcePath);
            if (!source.open(QIODevice::ReadOnly))
            {
                result.error = QStringLiteral("output/source-unavailable");
                return;
            }
            bytes = source.readAll();
            if (source.error() != QFile::NoError || hash(bytes) != QString::fromLatin1(request.document->getSourceDataHash().toHex()))
            {
                result.error = QStringLiteral("output/source-changed");
                return;
            }
        }
        else if (request.publicationReceipt.value(QStringLiteral("status")).toString() != QLatin1String("published") ||
                 request.publicationReceipt.value(QStringLiteral("published_sha256")).toString() != hash(bytes))
        {
            result.error = QStringLiteral("output/publication-identity-mismatch");
            return;
        }
        QFile output(result.stagedPath);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.flush())
        {
            result.error = QStringLiteral("output/staging-write-failed");
            return;
        }
        result.record.insert(QStringLiteral("publicationReceipt"), request.publicationReceipt);
        result.record.insert(QStringLiteral("renderer"), QStringLiteral("none: exact PDF byte copy"));
    }
    else
    {
        const auto* catalog = request.document->getCatalog();
        const bool printing = request.format == OutputFormat::PrintPdf;
        const int first = printing ? 0 : request.page;
        const int last = printing ? int(catalog->getPageCount()) - 1 : request.page;
        if (first < 0 || last >= int(catalog->getPageCount()) || first > last)
        {
            result.error = QStringLiteral("output/invalid-page");
            return;
        }
        pdf::PDFDocumentContext context(request.document);
        auto* session = context.getSession();
        pdf::PDFOptionalContentActivity optionalContent(request.document.data(), printing ? pdf::OCUsage::Print : pdf::OCUsage::Export, nullptr);
        pdf::PDFInkMapper inks(nullptr, request.document.data());
        inks.createSpotColors(true);
        pdf::PDFTransparencyRendererSettings settings;
        settings.renderPolicy = pdf::PDFRenderPolicy::forOutputPreview();
        settings.flags.setFlag(pdf::PDFTransparencyRendererSettings::SeparationSimulation, true);
        settings.flags.setFlag(pdf::PDFTransparencyRendererSettings::SmoothImageTransformation, true);
        pdf::PDFRenderDiagnostics diagnostics;
        std::unique_ptr<QPdfWriter> writer;
        QPainter painter;
        if (printing)
        {
            writer = std::make_unique<QPdfWriter>(result.stagedPath);
            writer->setResolution(request.dpi);
            writer->setTitle(QStringLiteral("Loop print-to-PDF"));
            writer->setCreator(QStringLiteral("Loop / PDFTransparencyRenderer"));
        }
        for (int index = first; index <= last; ++index)
        {
            if (job.isCancellationRequested())
                return;
            const auto* page = catalog->getPage(size_t(index));
            if (!page->getAnnotations().empty())
            {
                diagnostics.record(pdf::PDFRenderFidelity::Unsupported, QStringLiteral("Annotation appearances are not included in this page-content output."));
            }
            const QSizeF points = page->getRotatedMediaBox().size();
            if (!points.isValid() || !std::isfinite(points.width()) || !std::isfinite(points.height()) ||
                points.width() * request.dpi / 72.0 > 20000 || points.height() * request.dpi / 72.0 > 20000)
            {
                result.error = QStringLiteral("output/invalid-page-size");
                return;
            }
            const QSize pixels(qCeil(points.width() * request.dpi / 72.0), qCeil(points.height() * request.dpi / 72.0));
            job.processingBudget().chargeRenderPixels(uint64_t(pixels.width()) * uint64_t(pixels.height()), QStringLiteral("Quick output"));
            const QTransform matrix = pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, QRectF(QPointF(), QSizeF(pixels)));
            OutputRenderer renderer(page, request.document.data(), session->getFontCache(), session->getCMS(),
                                    &optionalContent, &inks, settings, matrix);
            renderer.setOperationControl(job.operationControl());
            renderer.beginPaint(pixels);
            renderer.processContents();
            renderer.endPaint();
            for (const auto& error : renderer.getRenderErrors())
            {
                if (error.type == pdf::RenderErrorType::Error)
                {
                    result.error = QStringLiteral("output/render-failed: %1").arg(error.message);
                    return;
                }
                if (error.type != pdf::RenderErrorType::Information)
                    diagnostics.record(pdf::PDFRenderFidelity::Unsupported, error.message);
            }
            diagnostics.merge(renderer.getRenderDiagnostics());
            if (!diagnostics.isExact() && !request.fidelityAcknowledged)
            {
                result.error = QStringLiteral("output/fidelity-acknowledgment-required");
                return;
            }
            QImage image = renderer.toImage(false, true, pdf::PDFRGB{ 1.0f, 1.0f, 1.0f });
            if (image.isNull())
            {
                result.error = QStringLiteral("output/render-failed");
                return;
            }
            image.setDotsPerMeterX(qRound(request.dpi / 0.0254));
            image.setDotsPerMeterY(qRound(request.dpi / 0.0254));
            if (printing)
            {
                writer->setPageSize(QPageSize(points, QPageSize::Point));
                writer->setPageMargins(QMarginsF(), QPageLayout::Point);
                if ((index == first && !painter.begin(writer.get())) || (index != first && !writer->newPage()))
                {
                    result.error = QStringLiteral("output/pdf-write-failed");
                    return;
                }
                painter.drawImage(QRect(0, 0, writer->width(), writer->height()), image);
            }
            else
            {
                QImageWriter imageWriter(result.stagedPath, request.format == OutputFormat::Png ? QByteArray("png") : QByteArray("tiff"));
                imageWriter.setText(QStringLiteral("Renderer"), QStringLiteral("Loop Core PDFTransparencyRenderer"));
                imageWriter.setText(QStringLiteral("Color handling"), QStringLiteral("Core CMS; RGB composite; spot separation simulation; white paper"));
                if (!imageWriter.write(image))
                {
                    result.error = QStringLiteral("output/image-encoder-unavailable-or-failed");
                    return;
                }
            }
            job.reportProgress((index - first + 1) * 90 / (last - first + 1));
        }
        if (printing && !painter.end())
        {
            result.error = QStringLiteral("output/pdf-write-failed");
            return;
        }
        writer.reset();
        result.record.insert(QStringLiteral("renderer"), QStringLiteral("Loop Core PDFTransparencyRenderer"));
        result.record.insert(QStringLiteral("dpi"), request.dpi);
        result.record.insert(QStringLiteral("firstPage"), first + 1);
        result.record.insert(QStringLiteral("lastPage"), last + 1);
        result.record.insert(QStringLiteral("colorHandling"), QStringLiteral("Core CMS; RGB composite; spot separation simulation; white paper"));
        result.record.insert(QStringLiteral("fidelity"), diagnostics.isExact() ? QStringLiteral("exact-supported") : QStringLiteral("limited"));
        result.record.insert(QStringLiteral("limitations"), QJsonArray::fromStringList(diagnostics.reasons));
    }
    QFile file(result.stagedPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        result.error = QStringLiteral("output/staging-read-failed");
        return;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&file))
    {
        result.error = QStringLiteral("output/staging-read-failed");
        return;
    }
    pdf::PDFArtifactIdentity artifact;
    artifact.sha256 = QString::fromLatin1(digest.result().toHex());
    artifact.size = file.size();
    artifact.mediaType = request.format == OutputFormat::Png ? QStringLiteral("image/png") : request.format == OutputFormat::Tiff ? QStringLiteral("image/tiff")
                                                                                                                                  : QStringLiteral("application/pdf");
    result.record.insert(QStringLiteral("artifact"), artifact.toJson());
    job.setOutputArtifact(artifact);
}
}
