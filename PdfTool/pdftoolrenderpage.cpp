// MIT License

#include "pdftoolrenderpage.h"

#include "pdfconstants.h"
#include "pdffont.h"
#include "pdfsafefilewriter.h"
#include "pdftoolstructuredoutput.h"

#include <QCryptographicHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

namespace pdftool
{

namespace
{

static PDFToolRenderPageApplication s_renderPageApplication;
constexpr const char* COLOR_OUTPUT_IDENTITY = "loupe-default-rgb8";

QJsonArray boxToJson(const QRectF& rect)
{
    return QJsonArray{ rect.left(), rect.top(), rect.right(), rect.bottom() };
}

QJsonArray transformToJson(const QTransform& transform)
{
    return QJsonArray{ transform.m11(), transform.m12(), transform.m21(), transform.m22(), transform.dx(), transform.dy() };
}

}   // namespace

QString PDFToolRenderPageApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return "render-page";
        case Name:
            return PDFToolTranslationContext::tr("Render one page for STCH");
        case Description:
            return PDFToolTranslationContext::tr("Render a trusted BleedBox page with its page-to-device contract.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolAbstractApplication::Options PDFToolRenderPageApplication::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | DestructiveWrite | RenderPage;
}

PDFToolExitCode PDFToolRenderPageApplication::execute(const PDFToolOptions& options)
{
    if (options.document.isEmpty() || options.renderPageIndex < 0 || options.renderPageDpi <= 0 ||
        options.renderPageMaxRasterPixels <= 0 || options.renderPageOutput.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         QStringLiteral("render-page requires document, --page-index, positive --dpi, positive --max-raster-pixels, and --output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    const PDFToolExitCode outputValidation = validateDestructiveOutput(options, options.renderPageOutput);
    if (outputValidation != PDFToolExitCode::Success)
    {
        return outputValidation;
    }

    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    const pdf::PDFInteger pageIndex = options.renderPageIndex;
    if (pageIndex >= document.getCatalog()->getPageCount())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         QStringLiteral("page index is outside the document."));
        return PDFToolExitCode::InvalidInvocation;
    }

    const pdf::PDFPage* page = document.getCatalog()->getPage(pageIndex);
    const QRectF bleedBox = page->getBleedBox().normalized();
    const QRectF trimBox = page->getTrimBox().normalized();
    if (!bleedBox.isValid() || bleedBox.isEmpty() || !trimBox.isValid() || trimBox.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("render.geometry-missing"),
                         QStringLiteral("page requires non-empty BleedBox and TrimBox."));
        return PDFToolExitCode::InputError;
    }
    if (!bleedBox.contains(trimBox))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("render.geometry-invalid"),
                         QStringLiteral("BleedBox must contain TrimBox."));
        return PDFToolExitCode::InputError;
    }

    const QRectF rotatedMedia = page->getRotatedMediaBox().normalized();
    const qreal pointToPixel = qreal(options.renderPageDpi) / 72.0;
    const int width = qCeil(rotatedMedia.width() * pointToPixel);
    const int height = qCeil(rotatedMedia.height() * pointToPixel);
    if (width <= 0 || height <= 0 || qint64(width) * qint64(height) > options.renderPageMaxRasterPixels)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("render.pixel-cap-exceeded"),
                         QStringLiteral("render exceeds max-raster-pixels."));
        return PDFToolExitCode::InputError;
    }

    pdf::PDFOptionalContentActivity optionalContentActivity(&document, pdf::OCUsage::Export, nullptr);
    pdf::PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(&document);
    pdf::PDFCMSPointer cms = cmsManager.getCurrentCMS();
    pdf::PDFFontCache fontCache(pdf::DEFAULT_FONT_CACHE_LIMIT, pdf::DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    pdf::PDFModifiedDocument modifiedDocument(&document, &optionalContentActivity);
    fontCache.setDocument(modifiedDocument);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    pdf::PDFMeshQualitySettings meshQualitySettings;
    const pdf::PDFRenderer::Features features = pdf::PDFRenderer::Features(pdf::PDFRenderer::Antialiasing | pdf::PDFRenderer::TextAntialiasing);
    pdf::PDFRenderer renderer(&document, &fontCache, cms.get(), &optionalContentActivity, features, meshQualitySettings);

    const QRect imageRect(0, 0, width, height);
    const QTransform fullTransform = pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, imageRect);
    const QRect bleedPixels = fullTransform.mapRect(bleedBox).normalized().toAlignedRect().intersected(imageRect);
    if (bleedPixels.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("render.geometry-outside-image"),
                         QStringLiteral("BleedBox does not map into the rendered page."));
        return PDFToolExitCode::InputError;
    }

    QImage fullImage(QSize(width, height), QImage::Format_ARGB32_Premultiplied);
    fullImage.fill(Qt::white);
    QPainter painter(&fullImage);
    const QList<pdf::PDFRenderError> renderErrors = renderer.render(&painter, fullTransform, pageIndex);
    painter.end();
    for (const pdf::PDFRenderError& error : renderErrors)
    {
        if (error.type == pdf::RenderErrorType::Error)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("render.page-error"), error.message);
            return PDFToolExitCode::PartialOutput;
        }
    }

    const QImage outputImage = fullImage.copy(bleedPixels);
    QString writeError;
    bool outputWritten = false;
    if (!options.destructiveDryRun)
    {
        const pdf::PDFSafeFileWriter::OverwritePolicy overwritePolicy = options.destructiveOverwrite
                                                                            ? pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite
                                                                            : pdf::PDFSafeFileWriter::OverwritePolicy::Fail;
        const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeDevice(options.renderPageOutput, [&outputImage, &writeError](QIODevice* device)
                                                                                        {
            QImageWriter writer(device, QByteArray("png"));
            if (!writer.write(outputImage))
            {
                writeError = writer.errorString();
                return false;
            }
            return true; }, overwritePolicy);
        if (!writeResult)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("render.output-error"), writeError.isEmpty() ? writeResult.getErrorMessage() : writeError);
            return PDFToolExitCode::ProcessingFailure;
        }
        outputWritten = true;
    }

    QTransform outputTransform = fullTransform;
    outputTransform.setMatrix(fullTransform.m11(), fullTransform.m12(), fullTransform.m13(),
                              fullTransform.m21(), fullTransform.m22(), fullTransform.m23(),
                              fullTransform.m31() - bleedPixels.left(), fullTransform.m32() - bleedPixels.top(), fullTransform.m33());
    QJsonObject data{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("command"), QStringLiteral("render-page") },
        { QStringLiteral("document_sha256"), QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex()) },
        { QStringLiteral("page_index"), static_cast<qint64>(pageIndex) },
        { QStringLiteral("reference_box"), QStringLiteral("bleed") },
        { QStringLiteral("reference_box_pt"), boxToJson(bleedBox) },
        { QStringLiteral("trim_box_pt"), boxToJson(trimBox) },
        { QStringLiteral("rotation_deg"), static_cast<int>(page->getPageRotation()) * 90 },
        { QStringLiteral("dpi"), options.renderPageDpi },
        { QStringLiteral("max_raster_pixels"), options.renderPageMaxRasterPixels },
        { QStringLiteral("pixel_size"), QJsonArray{ outputImage.width(), outputImage.height() } },
        { QStringLiteral("page_to_device"), transformToJson(outputTransform) },
        { QStringLiteral("renderer_features"), static_cast<qint64>(features.toInt()) },
        { QStringLiteral("color_output_identity"), QString::fromLatin1(COLOR_OUTPUT_IDENTITY) },
        { QStringLiteral("output"), options.renderPageOutput },
        { QStringLiteral("output_state"), outputWritten ? QStringLiteral("written") : QStringLiteral("planned") },
    };
    if (options.outputStyle == PDFOutputFormatter::Style::Json && options.executionContext)
    {
        options.executionContext->setData(data);
        options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("render-page"), options.renderPageOutput,
                                              outputWritten ? QStringLiteral("written") : QStringLiteral("planned") });
    }
    else
    {
        PDFConsole::writeText(formatStructuredObject(data, options.outputStyle, QStringLiteral("render-page")), options.outputCodec);
    }
    return PDFToolExitCode::Success;
}

}   // namespace pdftool
