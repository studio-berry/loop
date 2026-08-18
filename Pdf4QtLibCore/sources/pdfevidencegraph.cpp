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

#include "pdfevidencegraph.h"

#include "pdfblendfunction.h"
#include "pdfcatalog.h"
#include "pdfcms.h"
#include "pdfcolorinventory.h"
#include "pdfcolorspaces.h"
#include "pdfconstants.h"
#include "pdfdocumentsession.h"
#include "pdfexception.h"
#include "pdffont.h"
#include "pdfimage.h"
#include "pdfoptionalcontent.h"
#include "pdfpagecontentprocessor.h"
#include "pdfpattern.h"
#include "pdfpreflightchecks.h"
#include "pdfprocessingbudget.h"
#include "pdfschemaversion.h"

#include <QJsonArray>
#include <QPainterPathStroker>
#include <QSet>

#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace pdf
{

QString pdfEvidenceDomainToString(PDFEvidenceDomain domain)
{
    switch (domain)
    {
        case PDFEvidenceDomain::Images:
            return QStringLiteral("images");
        case PDFEvidenceDomain::Colorants:
            return QStringLiteral("colorants");
        case PDFEvidenceDomain::Strokes:
            return QStringLiteral("strokes");
        case PDFEvidenceDomain::OverprintTransparency:
            return QStringLiteral("overprint-transparency");
        case PDFEvidenceDomain::Fonts:
            return QStringLiteral("fonts");
    }
    return QStringLiteral("unknown");
}

PDFEvidenceDomains pdfEvidenceAllDomains()
{
    return PDFEvidenceDomains(PDFEvidenceDomain::Images) | PDFEvidenceDomain::Colorants | PDFEvidenceDomain::Strokes | PDFEvidenceDomain::OverprintTransparency | PDFEvidenceDomain::Fonts;
}

QJsonObject PDFEvidenceRecord::toJson() const
{
    QJsonObject object{
        { QStringLiteral("id"), id },
        { QStringLiteral("producer"), producer },
        { QStringLiteral("producer_version"), producerVersion },
        { QStringLiteral("domain"), pdfEvidenceDomainToString(domain) },
        { QStringLiteral("page"), page },
        { QStringLiteral("object_id"), objectId },
        { QStringLiteral("target"), target },
        { QStringLiteral("observed_value"), observedValue },
        { QStringLiteral("units"), units },
        { QStringLiteral("coverage_method"), coverageMethod },
        { QStringLiteral("fidelity"), fidelity },
        { QStringLiteral("confidence"), confidence },
        { QStringLiteral("incomplete_reason"), incompleteReason },
        { QStringLiteral("budget_context"), budgetContext }
    };
    if (!extra.isEmpty())
    {
        object.insert(QStringLiteral("extra"), extra);
    }
    return object;
}

QList<PDFEvidenceRecord> PDFEvidenceGraph::recordsForDomain(PDFEvidenceDomain domain) const
{
    QList<PDFEvidenceRecord> matched;
    for (const PDFEvidenceRecord& record : records)
    {
        if (record.domain == domain)
        {
            matched.append(record);
        }
    }
    return matched;
}

QList<PDFEvidenceRecord> PDFEvidenceGraph::recordsForTarget(PDFEvidenceDomain domain, const QString& target) const
{
    QList<PDFEvidenceRecord> matched;
    for (const PDFEvidenceRecord& record : records)
    {
        if (record.domain == domain && record.target == target)
        {
            matched.append(record);
        }
    }
    return matched;
}

QJsonObject PDFEvidenceGraph::toJson() const
{
    QJsonArray items;
    for (const PDFEvidenceRecord& record : records)
    {
        items.append(record.toJson());
    }
    QJsonObject object{
        { QStringLiteral("producer"), producer },
        { QStringLiteral("producer_version"), producerVersion },
        { QStringLiteral("complete"), isComplete() },
        { QStringLiteral("incomplete_reason"), incompleteReason },
        { QStringLiteral("records"), items }
    };
    writeSchemaEnvelope(object, PDFSchemaKind::EvidenceGraph, PDFSchemaVersion{ 1, 0 });
    return object;
}

namespace
{

constexpr int EVIDENCE_MAX_FORM_DEPTH = 32;

PDFEvidenceRecord makeRecord(const PDFEvidenceGraph* graph,
                             PDFEvidenceDomain domain,
                             int pageNumber,
                             const QString& target)
{
    PDFEvidenceRecord record;
    record.producer = graph->producer;
    record.producerVersion = graph->producerVersion;
    record.artifact = graph->artifact;
    record.revision = graph->revision;
    record.domain = domain;
    record.page = pageNumber;
    record.target = target;
    record.coverageMethod = QStringLiteral("content-stream");
    record.fidelity = QStringLiteral("exact");
    record.confidence = 1.0;
    return record;
}

void throwIfContentProcessingIncomplete(const QList<PDFRenderError>& errors)
{
    for (const PDFRenderError& error : errors)
    {
        if (error.type == RenderErrorType::Error || error.type == RenderErrorType::NotImplemented || error.type == RenderErrorType::NotSupported)
        {
            throw PDFException(error.message);
        }
    }
}

void appendEvidenceRecord(PDFEvidenceGraph* graph, PDFEvidenceRecord record, PDFProcessingBudget* budget)
{
    if (budget)
    {
        budget->chargeEvidenceRecords(1, QStringLiteral("evidence-graph"));
    }
    graph->records.append(std::move(record));
}

void processAnnotationAppearanceStreams(PDFDocument* document,
                                        const PDFPage* page,
                                        const std::function<void(const PDFStream*)>& processForm)
{
    if (!document || !page)
    {
        return;
    }

    for (const PDFObjectReference& annotRef : page->getAnnotations())
    {
        PDFObject annotObject = document->getObjectByReference(annotRef);
        if (!annotObject.isDictionary())
        {
            continue;
        }

        const PDFDictionary* annotDict = annotObject.getDictionary();
        PDFObject appearance = document->getObject(annotDict->get("AP"));
        if (!appearance.isDictionary())
        {
            continue;
        }

        const PDFDictionary* apDict = appearance.getDictionary();
        for (size_t i = 0; i < apDict->getCount(); ++i)
        {
            PDFObject appearanceStream = document->getObject(apDict->getValue(i));
            if (appearanceStream.isDictionary())
            {
                const PDFDictionary* stateDict = appearanceStream.getDictionary();
                for (size_t j = 0; j < stateDict->getCount(); ++j)
                {
                    PDFObject nested = document->getObject(stateDict->getValue(j));
                    if (nested.isStream())
                    {
                        processForm(nested.getStream());
                    }
                }
            }
            else if (appearanceStream.isStream())
            {
                processForm(appearanceStream.getStream());
            }
        }
    }
}

QString classifyPaintedColorSpace(const PDFAbstractColorSpace* colorSpace)
{
    if (!colorSpace)
    {
        return QString();
    }

    const PDFAbstractColorSpace* base = colorSpace;
    while (base && base->getColorSpace() == PDFAbstractColorSpace::ColorSpace::Indexed)
    {
        base = static_cast<const PDFIndexedColorSpace*>(base)->getBaseColorSpace().get();
    }
    if (!base)
    {
        return QString();
    }

    switch (base->getColorSpace())
    {
        case PDFAbstractColorSpace::ColorSpace::DeviceRGB:
            return QStringLiteral("DeviceRGB");
        case PDFAbstractColorSpace::ColorSpace::DeviceCMYK:
            return QStringLiteral("DeviceCMYK");
        case PDFAbstractColorSpace::ColorSpace::DeviceGray:
            return QStringLiteral("DeviceGray");
        case PDFAbstractColorSpace::ColorSpace::CalRGB:
        case PDFAbstractColorSpace::ColorSpace::ICCBased:
        case PDFAbstractColorSpace::ColorSpace::Lab:
            return QStringLiteral("DeviceRGB");
        case PDFAbstractColorSpace::ColorSpace::CalGray:
            return QStringLiteral("DeviceGray");
        case PDFAbstractColorSpace::ColorSpace::DeviceN:
        case PDFAbstractColorSpace::ColorSpace::Separation:
            return QStringLiteral("DeviceCMYK");
        default:
            return QString();
    }
}

void recordPaintedColorSpace(const PDFAbstractColorSpace* colorSpace, QSet<QString>* paintedSpaces)
{
    if (!paintedSpaces)
    {
        return;
    }
    const QString name = classifyPaintedColorSpace(colorSpace);
    if (!name.isEmpty())
    {
        paintedSpaces->insert(name);
    }
}

void recordPaintedImageColorSpace(const PDFImage& image, QSet<QString>* paintedSpaces)
{
    if (!paintedSpaces)
    {
        return;
    }
    const QString name = classifyPaintedColorSpace(image.getColorSpace().get());
    if (!name.isEmpty())
    {
        paintedSpaces->insert(name);
        return;
    }
    switch (image.getImageData().getComponents())
    {
        case 1:
            paintedSpaces->insert(QStringLiteral("DeviceGray"));
            break;
        case 3:
            paintedSpaces->insert(QStringLiteral("DeviceRGB"));
            break;
        case 4:
            paintedSpaces->insert(QStringLiteral("DeviceCMYK"));
            break;
        default:
            break;
    }
}

void recordColorSpaceObject(const PDFDocument* document,
                            const PDFDictionary* colorSpaceDictionary,
                            const PDFObject& colorSpaceObject,
                            QSet<QString>* paintedSpaces)
{
    if (!document || !paintedSpaces)
    {
        return;
    }
    if (!colorSpaceObject.isName() && !colorSpaceObject.isArray())
    {
        return;
    }
    try
    {
        const PDFColorSpacePointer colorSpace = PDFAbstractColorSpace::createColorSpace(
            colorSpaceDictionary, document, colorSpaceObject);
        recordPaintedColorSpace(colorSpace.get(), paintedSpaces);
    }
    catch (const PDFException&)
    {
    }
}

void collectColorSpacesFromResources(const PDFDocument* document,
                                     const PDFObject& resourcesObject,
                                     QSet<QString>* paintedSpaces,
                                     std::set<PDFObjectReference>& visitedForms,
                                     int depth)
{
    if (!document || !paintedSpaces || depth > EVIDENCE_MAX_FORM_DEPTH)
    {
        return;
    }
    const PDFObject resources = document->getObject(resourcesObject);
    if (!resources.isDictionary())
    {
        return;
    }
    const PDFDictionary* resourcesDict = resources.getDictionary();
    const PDFDictionary* colorSpaceDictionary = document->getDictionaryFromObject(resourcesDict->get("ColorSpace"));
    if (colorSpaceDictionary)
    {
        for (size_t i = 0; i < colorSpaceDictionary->getCount(); ++i)
        {
            recordColorSpaceObject(document,
                                   colorSpaceDictionary,
                                   document->getObject(colorSpaceDictionary->getValue(i)),
                                   paintedSpaces);
        }
    }
    const PDFDictionary* xobjectDict = document->getDictionaryFromObject(resourcesDict->get("XObject"));
    if (!xobjectDict)
    {
        return;
    }
    PDFDocumentDataLoaderDecorator loader(document);
    for (size_t i = 0; i < xobjectDict->getCount(); ++i)
    {
        const PDFObject& entry = xobjectDict->getValue(i);
        const PDFObject xobject = document->getObject(entry);
        if (!xobject.isStream())
        {
            continue;
        }
        const PDFDictionary* streamDict = xobject.getStream()->getDictionary();
        if (!streamDict)
        {
            continue;
        }
        const QByteArray subtype = loader.readNameFromDictionary(streamDict, "Subtype");
        if (subtype == "Image")
        {
            if (streamDict->hasKey("ColorSpace"))
            {
                recordColorSpaceObject(document,
                                       colorSpaceDictionary,
                                       document->getObject(streamDict->get("ColorSpace")),
                                       paintedSpaces);
            }
        }
        else if (subtype == "Form")
        {
            if (entry.isReference() && !visitedForms.insert(entry.getReference()).second)
            {
                continue;
            }
            collectColorSpacesFromResources(document,
                                            streamDict->get("Resources"),
                                            paintedSpaces,
                                            visitedForms,
                                            depth + 1);
        }
    }
}

QRectF imageBoundsFromCtm(const QTransform& ctm)
{
    // An image's CTM maps the unit square (0,0)-(1,1) to placement space; the
    // image's pixel dimensions affect resolution/DPI only, never placement geometry.
    const QPointF corners[4] = {
        ctm.map(QPointF(0, 0)),
        ctm.map(QPointF(1, 0)),
        ctm.map(QPointF(1, 1)),
        ctm.map(QPointF(0, 1))
    };
    QRectF bounds;
    for (const QPointF& corner : corners)
    {
        bounds |= QRectF(corner, corner);
    }
    return bounds.normalized();
}

bool isNearWhiteDevicePaint(const PDFAbstractColorSpace* colorSpace, const PDFColor& color, int recursionDepth = 0)
{
    if (!colorSpace || recursionDepth > 8)
    {
        return false;
    }
    switch (colorSpace->getColorSpace())
    {
        case PDFAbstractColorSpace::ColorSpace::DeviceGray:
            return color[0] >= 0.99f;
        case PDFAbstractColorSpace::ColorSpace::DeviceRGB:
            return color[0] >= 0.99f && color[1] >= 0.99f && color[2] >= 0.99f;
        case PDFAbstractColorSpace::ColorSpace::DeviceCMYK:
            return color[0] <= 0.01f && color[1] <= 0.01f && color[2] <= 0.01f && color[3] <= 0.01f;
        case PDFAbstractColorSpace::ColorSpace::ICCBased:
        {
            const PDFICCBasedColorSpace* iccColorSpace = static_cast<const PDFICCBasedColorSpace*>(colorSpace);
            return isNearWhiteDevicePaint(iccColorSpace->getAlternateColorSpace(), color, recursionDepth + 1);
        }
        case PDFAbstractColorSpace::ColorSpace::Separation:
        case PDFAbstractColorSpace::ColorSpace::DeviceN:
        {
            std::vector<PDFColorComponent> input(color.size());
            for (size_t i = 0; i < color.size(); ++i)
            {
                input[i] = color[i];
            }
            PDFColorSpacePointer alternateColorSpace;
            std::vector<PDFColorComponent> transformed;
            if (colorSpace->getColorSpace() == PDFAbstractColorSpace::ColorSpace::Separation)
            {
                const PDFSeparationColorSpace* separationColorSpace = static_cast<const PDFSeparationColorSpace*>(colorSpace);
                alternateColorSpace = separationColorSpace->getAlternateColorSpace();
                transformed = separationColorSpace->transformColorsToBaseColorSpace(PDFColorBuffer(input.data(), input.size()));
            }
            else
            {
                const PDFDeviceNColorSpace* deviceNColorSpace = static_cast<const PDFDeviceNColorSpace*>(colorSpace);
                alternateColorSpace = deviceNColorSpace->getAlternateColorSpace();
                transformed = deviceNColorSpace->transformColorsToBaseColorSpace(PDFColorBuffer(input.data(), input.size()));
            }
            PDFColor alternateColor;
            alternateColor.resize(transformed.size());
            for (size_t i = 0; i < transformed.size(); ++i)
            {
                alternateColor[i] = transformed[i];
            }
            return isNearWhiteDevicePaint(alternateColorSpace.data(), alternateColor, recursionDepth + 1);
        }
        default:
            return false;
    }
}

enum class TransparencyColorFamily
{
    Unknown,
    Gray,
    RGB,
    CMYK,
    Spot
};

QString transparencyColorFamilyName(TransparencyColorFamily family)
{
    switch (family)
    {
        case TransparencyColorFamily::Gray:
            return QStringLiteral("Gray");
        case TransparencyColorFamily::RGB:
            return QStringLiteral("RGB");
        case TransparencyColorFamily::CMYK:
            return QStringLiteral("CMYK");
        case TransparencyColorFamily::Spot:
            return QStringLiteral("Spot");
        default:
            return QStringLiteral("Unknown");
    }
}

TransparencyColorFamily classifyTransparencyColorSpace(const PDFAbstractColorSpace* colorSpace)
{
    if (!colorSpace)
    {
        return TransparencyColorFamily::Unknown;
    }
    const PDFAbstractColorSpace* base = colorSpace;
    while (base && base->getColorSpace() == PDFAbstractColorSpace::ColorSpace::Indexed)
    {
        base = static_cast<const PDFIndexedColorSpace*>(base)->getBaseColorSpace().get();
    }
    if (!base)
    {
        return TransparencyColorFamily::Unknown;
    }
    switch (base->getColorSpace())
    {
        case PDFAbstractColorSpace::ColorSpace::DeviceGray:
        case PDFAbstractColorSpace::ColorSpace::CalGray:
            return TransparencyColorFamily::Gray;
        case PDFAbstractColorSpace::ColorSpace::DeviceRGB:
        case PDFAbstractColorSpace::ColorSpace::CalRGB:
        case PDFAbstractColorSpace::ColorSpace::Lab:
            return TransparencyColorFamily::RGB;
        case PDFAbstractColorSpace::ColorSpace::DeviceCMYK:
            return TransparencyColorFamily::CMYK;
        case PDFAbstractColorSpace::ColorSpace::ICCBased:
        {
            const PDFICCBasedColorSpace* icc = static_cast<const PDFICCBasedColorSpace*>(base);
            return classifyTransparencyColorSpace(icc->getAlternateColorSpace());
        }
        case PDFAbstractColorSpace::ColorSpace::Separation:
        case PDFAbstractColorSpace::ColorSpace::DeviceN:
            return TransparencyColorFamily::Spot;
        default:
            return TransparencyColorFamily::Unknown;
    }
}

bool isRiskyTransparencyConversion(TransparencyColorFamily blendSpace, TransparencyColorFamily sourceSpace)
{
    if (blendSpace == TransparencyColorFamily::Unknown || sourceSpace == TransparencyColorFamily::Unknown || blendSpace == sourceSpace)
    {
        return false;
    }
    if (sourceSpace == TransparencyColorFamily::Gray)
    {
        return false;
    }
    if (sourceSpace == TransparencyColorFamily::Spot)
    {
        return true;
    }
    if (blendSpace == TransparencyColorFamily::Gray)
    {
        return true;
    }
    return (blendSpace == TransparencyColorFamily::RGB && sourceSpace == TransparencyColorFamily::CMYK) || (blendSpace == TransparencyColorFamily::CMYK && sourceSpace == TransparencyColorFamily::RGB);
}

class EvidenceProcessor : public PDFPageContentProcessor
{
public:
    EvidenceProcessor(const PDFPage* page,
                      const PDFDocument* document,
                      const PDFFontCache* fontCache,
                      const PDFCMS* cms,
                      const PDFOptionalContentActivity* optionalContent,
                      const PDFMeshQualitySettings& meshQuality,
                      PDFProcessingBudget* budget,
                      PDFEvidenceGraph* graph,
                      PDFEvidenceDomains domains,
                      const PDFEvidenceCollectSettings& settings,
                      int pageNumber,
                      QSet<QString>* paintedSpaces,
                      bool* foundWhiteOverprint,
                      QSet<QString>* riskyBlendModes,
                      QSet<QString>* mismatchDescriptions) :
        PDFPageContentProcessor(page, document, fontCache, cms, optionalContent, QTransform(), meshQuality, budget),
        m_graph(graph),
        m_domains(domains),
        m_settings(settings),
        m_pageNumber(pageNumber),
        m_paintedSpaces(paintedSpaces),
        m_foundWhiteOverprint(foundWhiteOverprint),
        m_riskyBlendModes(riskyBlendModes),
        m_mismatchDescriptions(mismatchDescriptions),
        m_budget(budget)
    {
        QRectF pageClip = page ? page->getCropBox().normalized() : QRectF();
        if (pageClip.isEmpty() && page)
        {
            pageClip = page->getMediaBox().normalized();
        }
        if (!pageClip.isEmpty())
        {
            m_clipPath.addRect(pageClip);
        }
    }

    void processFormStream(const PDFStream* stream)
    {
        if (stream && !isContentSuppressed())
        {
            processForm(stream);
        }
    }

    const QList<PDFRenderError>& renderErrors() const { return getRenderErrors(); }

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        Q_UNUSED(kind);
        return false;
    }

    bool performOriginalImagePainting(const PDFImage& image,
                                      const PDFStream* stream,
                                      PDFObjectReference reference) override
    {
        if (isContentSuppressed())
        {
            return true;
        }

        if (m_domains.testFlag(PDFEvidenceDomain::Colorants))
        {
            if (stream)
            {
                const PDFDictionary* streamDictionary = stream->getDictionary();
                if (streamDictionary && streamDictionary->hasKey("ColorSpace"))
                {
                    recordColorSpaceObject(getDocument(),
                                           getColorSpaceDictionary(),
                                           getDocument()->getObject(streamDictionary->get("ColorSpace")),
                                           m_paintedSpaces);
                }
            }
            recordPaintedImageColorSpace(image, m_paintedSpaces);
        }

        if (m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency) && !m_groups.empty())
        {
            recordPaintedImageSpace(image);
        }

        if (!m_domains.testFlag(PDFEvidenceDomain::Images))
        {
            return true;
        }

        const QTransform ctm = getGraphicState()->getCurrentTransformationMatrix();
        const auto axisLength = [](qreal x, qreal y)
        {
            return std::hypot(static_cast<double>(x), static_cast<double>(y)) * PDF_POINT_TO_INCH;
        };
        const double widthInches = axisLength(ctm.m11(), ctm.m12());
        const double heightInches = axisLength(ctm.m21(), ctm.m22());
        if (widthInches <= std::numeric_limits<double>::epsilon() || heightInches <= std::numeric_limits<double>::epsilon())
        {
            return true;
        }

        PDFEvidenceRecord record = makeRecord(m_graph, PDFEvidenceDomain::Images, m_pageNumber, QStringLiteral("image-effective-dpi"));
        record.objectId = reference.isValid() ? QString::number(reference.objectNumber) : QString();
        record.observedValue = qMin(qreal(image.getImageData().getWidth()) / widthInches,
                                    qreal(image.getImageData().getHeight()) / heightInches);
        record.units = QStringLiteral("dpi");
        record.geometry = imageBoundsFromCtm(ctm);
        record.id = QStringLiteral("img:%1:%2:%3").arg(m_pageNumber).arg(record.objectId.isEmpty() ? QStringLiteral("anon") : record.objectId).arg(++m_imageOrdinal);
        appendEvidenceRecord(m_graph, record, m_budget);
        return true;
    }

    void performPathPainting(const QPainterPath& path, bool stroke, bool fill, bool text, Qt::FillRule fillRule) override
    {
        Q_UNUSED(path);
        Q_UNUSED(text);
        Q_UNUSED(fillRule);
        if (isContentSuppressed())
        {
            return;
        }
        const PDFPageContentProcessorState* state = getGraphicState();
        if (m_domains.testFlag(PDFEvidenceDomain::Colorants))
        {
            if (fill)
            {
                recordPaintedColorSpace(state->getFillColorSpace(), m_paintedSpaces);
            }
            if (stroke)
            {
                recordPaintedColorSpace(state->getStrokeColorSpace(), m_paintedSpaces);
            }
        }
        if (m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
        {
            const PDFOverprintMode overprintMode = state->getOverprintMode();
            if (m_foundWhiteOverprint)
            {
                if (fill && overprintMode.overprintFilling && isNearWhiteDevicePaint(state->getFillColorSpace(), state->getFillColorOriginal()))
                {
                    *m_foundWhiteOverprint = true;
                }
                if (stroke && overprintMode.overprintStroking && isNearWhiteDevicePaint(state->getStrokeColorSpace(), state->getStrokeColorOriginal()))
                {
                    *m_foundWhiteOverprint = true;
                }
            }
            if (!m_groups.empty())
            {
                if (fill)
                {
                    recordPaintedSpace(state->getFillColorSpace());
                }
                if (stroke)
                {
                    recordPaintedSpace(state->getStrokeColorSpace());
                }
            }
        }
    }

    void performBeforePathPainting(const QPainterPath& path,
                                   bool stroke,
                                   bool fill,
                                   bool text,
                                   Qt::FillRule fillRule) override
    {
        Q_UNUSED(fill);
        Q_UNUSED(text);
        Q_UNUSED(fillRule);
        if (!stroke || path.isEmpty() || !m_domains.testFlag(PDFEvidenceDomain::Strokes))
        {
            return;
        }
        const PDFPageContentProcessorState* state = getGraphicState();
        const qreal declaredWidth = state->getLineWidth();
        const qreal effectiveWidth = preflight::minimumEffectiveStrokeWidth(
            path,
            declaredWidth,
            state->getCurrentTransformationMatrix(),
            getPage() ? getPage()->getUserUnit() : 1.0);
        const bool hairline = declaredWidth <= m_settings.zeroWidthEpsilonPt;

        QPainterPathStroker stroker;
        stroker.setWidth(std::max(std::abs(declaredWidth), m_settings.zeroWidthEpsilonPt));
        stroker.setCapStyle(state->getLineCapStyle());
        stroker.setJoinStyle(state->getLineJoinStyle());
        stroker.setMiterLimit(state->getMitterLimit());
        const PDFLineDashPattern& dash = state->getLineDashPattern();
        if (!dash.isSolid())
        {
            stroker.setDashPattern(dash.createForQPen(std::max(std::abs(declaredWidth), m_settings.zeroWidthEpsilonPt)));
            stroker.setDashOffset(dash.getDashOffset());
        }
        QPainterPath visibleStroke = getCurrentWorldMatrix().map(stroker.createStroke(path));
        if (!m_clipPath.isEmpty())
        {
            visibleStroke = visibleStroke.intersected(m_clipPath);
        }
        if (visibleStroke.isEmpty())
        {
            return;
        }

        PDFEvidenceRecord record = makeRecord(m_graph, PDFEvidenceDomain::Strokes, m_pageNumber,
                                              hairline ? QStringLiteral("hairline-stroke") : QStringLiteral("stroke-width"));
        record.observedValue = effectiveWidth;
        record.units = QStringLiteral("pt");
        record.geometry = visibleStroke.boundingRect();
        record.extra.insert(QStringLiteral("declared_width"), declaredWidth);
        record.extra.insert(QStringLiteral("effective_width"), effectiveWidth);
        record.extra.insert(QStringLiteral("hairline"), hairline);
        record.id = QStringLiteral("stroke:%1:%2").arg(m_pageNumber).arg(++m_strokeOrdinal);
        appendEvidenceRecord(m_graph, record, m_budget);
    }

    void performClipping(const QPainterPath& path, Qt::FillRule fillRule) override
    {
        Q_UNUSED(fillRule);
        const QPainterPath pagePath = getCurrentWorldMatrix().map(path);
        m_clipPath = m_clipPath.isEmpty() ? pagePath : m_clipPath.intersected(pagePath);
    }

    void performSaveGraphicState(ProcessOrder order) override
    {
        if (order == ProcessOrder::AfterOperation)
        {
            m_clipStack.push_back(m_clipPath);
        }
    }

    void performRestoreGraphicState(ProcessOrder order) override
    {
        if (order == ProcessOrder::BeforeOperation && !m_clipStack.empty())
        {
            m_clipPath = m_clipStack.back();
            m_clipStack.pop_back();
        }
    }

    void performBeginTransparencyGroup(ProcessOrder order, const PDFTransparencyGroup& group) override
    {
        if (order != ProcessOrder::BeforeOperation || !m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
        {
            return;
        }
        TransparencyGroupFrame frame;
        frame.hasExplicitBlendSpace = !!group.colorSpacePointer;
        frame.blendSpace = classifyTransparencyColorSpace(group.colorSpacePointer.get());
        if (!frame.hasExplicitBlendSpace && !m_groups.empty())
        {
            frame.blendSpace = m_groups.back().blendSpace;
        }
        frame.entryBlendMode = getGraphicState()->getBlendMode();
        inspectGroupBlendMode(frame.entryBlendMode);
        m_groups.push_back(std::move(frame));
    }

    void performEndTransparencyGroup(ProcessOrder order, const PDFTransparencyGroup& group) override
    {
        Q_UNUSED(group);
        if (order != ProcessOrder::AfterOperation || m_groups.empty() || !m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
        {
            return;
        }
        TransparencyGroupFrame frame = std::move(m_groups.back());
        m_groups.pop_back();
        evaluateBlendSpace(frame);
        if (!m_groups.empty())
        {
            TransparencyColorFamily outputSpace = frame.blendSpace;
            if (outputSpace == TransparencyColorFamily::Unknown)
            {
                outputSpace = m_groups.back().blendSpace;
            }
            if (outputSpace != TransparencyColorFamily::Unknown)
            {
                m_groups.back().paintedSpaces.insert(outputSpace);
            }
        }
    }

    void performUpdateGraphicsState(const PDFPageContentProcessorState& state) override
    {
        if (m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency) && state.getStateFlags().testFlag(PDFPageContentProcessorState::StateBlendMode))
        {
            inspectBlendMode(state.getBlendMode());
        }
        PDFPageContentProcessor::performUpdateGraphicsState(state);
    }

    bool performPathPaintingUsingShading(const QPainterPath& path,
                                         bool stroke,
                                         bool fill,
                                         const PDFShadingPattern* shadingPattern) override
    {
        Q_UNUSED(path);
        Q_UNUSED(stroke);
        Q_UNUSED(fill);
        if (shadingPattern && !isContentSuppressed() && m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
        {
            recordPaintedSpace(shadingPattern->getColorSpace());
        }
        return false;
    }

private:
    struct TransparencyGroupFrame
    {
        TransparencyColorFamily blendSpace = TransparencyColorFamily::Unknown;
        bool hasExplicitBlendSpace = false;
        BlendMode entryBlendMode = BlendMode::Normal;
        QSet<TransparencyColorFamily> paintedSpaces;
    };

    void inspectBlendMode(BlendMode mode)
    {
        if (!m_riskyBlendModes || mode == BlendMode::Normal || mode == BlendMode::Compatible)
        {
            return;
        }
        if (mode == BlendMode::Invalid || !PDFBlendModeInfo::isSupportedByQt(mode))
        {
            m_riskyBlendModes->insert(PDFBlendModeInfo::getBlendModeName(mode));
        }
    }

    void inspectGroupBlendMode(BlendMode mode)
    {
        if (!m_riskyBlendModes || mode == BlendMode::Normal || mode == BlendMode::Compatible)
        {
            return;
        }
        QString description = PDFBlendModeInfo::getBlendModeName(mode);
        description += QStringLiteral(" (transparency group)");
        m_riskyBlendModes->insert(description);
    }

    void recordPaintedSpace(const PDFAbstractColorSpace* colorSpace)
    {
        if (m_groups.empty())
        {
            return;
        }
        const TransparencyColorFamily family = classifyTransparencyColorSpace(colorSpace);
        if (family != TransparencyColorFamily::Unknown)
        {
            m_groups.back().paintedSpaces.insert(family);
        }
    }

    void recordPaintedImageSpace(const PDFImage& image)
    {
        if (m_groups.empty())
        {
            return;
        }
        const TransparencyColorFamily family = classifyTransparencyColorSpace(image.getColorSpace().get());
        if (family != TransparencyColorFamily::Unknown)
        {
            m_groups.back().paintedSpaces.insert(family);
            return;
        }
        switch (image.getImageData().getComponents())
        {
            case 1:
                m_groups.back().paintedSpaces.insert(TransparencyColorFamily::Gray);
                break;
            case 3:
                m_groups.back().paintedSpaces.insert(TransparencyColorFamily::RGB);
                break;
            case 4:
                m_groups.back().paintedSpaces.insert(TransparencyColorFamily::CMYK);
                break;
            default:
                break;
        }
    }

    void evaluateBlendSpace(const TransparencyGroupFrame& frame)
    {
        if (!frame.hasExplicitBlendSpace || frame.blendSpace == TransparencyColorFamily::Unknown || !m_mismatchDescriptions)
        {
            return;
        }
        for (const TransparencyColorFamily source : frame.paintedSpaces)
        {
            if (isRiskyTransparencyConversion(frame.blendSpace, source))
            {
                m_mismatchDescriptions->insert(
                    QStringLiteral("%1 group contains %2 content")
                        .arg(transparencyColorFamilyName(frame.blendSpace), transparencyColorFamilyName(source)));
            }
        }
    }

    PDFEvidenceGraph* m_graph = nullptr;
    PDFEvidenceDomains m_domains;
    PDFEvidenceCollectSettings m_settings;
    int m_pageNumber = 1;
    QSet<QString>* m_paintedSpaces = nullptr;
    bool* m_foundWhiteOverprint = nullptr;
    QSet<QString>* m_riskyBlendModes = nullptr;
    QSet<QString>* m_mismatchDescriptions = nullptr;
    PDFProcessingBudget* m_budget = nullptr;
    QPainterPath m_clipPath;
    std::vector<QPainterPath> m_clipStack;
    std::vector<TransparencyGroupFrame> m_groups;
    int m_imageOrdinal = 0;
    int m_strokeOrdinal = 0;
};

void collectFonts(PDFDocument* document, PDFEvidenceGraph* graph, PDFProcessingBudget* budget)
{
    const PDFCatalog* catalog = document->getCatalog();
    std::set<PDFObjectReference> processedFonts;
    std::set<PDFObjectReference> processedResources;

    std::function<void(const PDFObject&, int)> scanResources;
    scanResources = [&](const PDFObject& resourcesObject, int pageNumber)
    {
        PDFObject resources = document->getObject(resourcesObject);
        if (!resources.isDictionary())
        {
            return;
        }
        if (resourcesObject.isReference())
        {
            const PDFObjectReference ref = resourcesObject.getReference();
            if (processedResources.contains(ref))
            {
                return;
            }
            processedResources.insert(ref);
        }

        const PDFDictionary* fontsDict = document->getDictionaryFromObject(resources.getDictionary()->get("Font"));
        if (fontsDict)
        {
            for (size_t i = 0; i < fontsDict->getCount(); ++i)
            {
                PDFObject fontObj = fontsDict->getValue(i);
                if (fontObj.isReference())
                {
                    const PDFObjectReference ref = fontObj.getReference();
                    if (processedFonts.contains(ref))
                    {
                        continue;
                    }
                    processedFonts.insert(ref);
                }
                try
                {
                    PDFFontPointer font = PDFFont::createFont(fontObj, fontsDict->getKey(i).getString(), document);
                    if (!font || font->getFontType() == FontType::Type3)
                    {
                        continue;
                    }
                    const FontDescriptor* fd = font->getFontDescriptor();
                    const QString keyName = QString::fromLatin1(fontsDict->getKey(i).getString());
                    PDFEvidenceRecord record = makeRecord(graph, PDFEvidenceDomain::Fonts, pageNumber, QStringLiteral("font-resource"));
                    record.coverageMethod = QStringLiteral("resource-dictionary");
                    record.fidelity = QStringLiteral("catalog");
                    record.objectId = keyName;
                    record.extra.insert(QStringLiteral("key_name"), keyName);
                    if (!fd)
                    {
                        record.extra.insert(QStringLiteral("embedded"), false);
                        record.extra.insert(QStringLiteral("missing_descriptor"), true);
                        record.extra.insert(QStringLiteral("font_name"), keyName);
                    }
                    else
                    {
                        record.extra.insert(QStringLiteral("embedded"), fd->isEmbedded());
                        record.extra.insert(QStringLiteral("font_name"), fd->fontName.isEmpty() ? keyName : fd->fontName);
                    }
                    record.id = QStringLiteral("font:%1:%2").arg(pageNumber).arg(keyName);
                    appendEvidenceRecord(graph, record, budget);
                }
                catch (const PDFException&)
                {
                }
            }
        }

        const PDFDictionary* xobjectDict = document->getDictionaryFromObject(resources.getDictionary()->get("XObject"));
        if (!xobjectDict)
        {
            return;
        }
        PDFDocumentDataLoaderDecorator loader(document);
        for (size_t i = 0; i < xobjectDict->getCount(); ++i)
        {
            PDFObject xobject = document->getObject(xobjectDict->getValue(i));
            if (!xobject.isStream())
            {
                continue;
            }
            const PDFDictionary* streamDict = xobject.getStream()->getDictionary();
            if (!streamDict)
            {
                continue;
            }
            if (loader.readNameFromDictionary(streamDict, "Subtype") != "Form")
            {
                continue;
            }
            if (streamDict->hasKey("Resources"))
            {
                scanResources(streamDict->get("Resources"), pageNumber);
            }
        }
    };

    for (PDFInteger pageIndex = 0; pageIndex < catalog->getPageCount(); ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (page)
        {
            const int pageNumber = int(pageIndex + 1);
            scanResources(page->getResources(), pageNumber);

            for (const PDFObjectReference& annotRef : page->getAnnotations())
            {
                PDFObject annotObject = document->getObjectByReference(annotRef);
                if (!annotObject.isDictionary())
                {
                    continue;
                }

                const PDFDictionary* annotDict = annotObject.getDictionary();
                PDFObject appearance = document->getObject(annotDict->get("AP"));
                if (!appearance.isDictionary())
                {
                    continue;
                }

                const PDFDictionary* apDict = appearance.getDictionary();
                for (size_t i = 0; i < apDict->getCount(); ++i)
                {
                    PDFObject appearanceStream = document->getObject(apDict->getValue(i));
                    if (appearanceStream.isDictionary())
                    {
                        const PDFDictionary* stateDict = appearanceStream.getDictionary();
                        for (size_t j = 0; j < stateDict->getCount(); ++j)
                        {
                            PDFObject nested = document->getObject(stateDict->getValue(j));
                            if (nested.isStream() && nested.getStream()->getDictionary()->hasKey("Resources"))
                            {
                                scanResources(nested.getStream()->getDictionary()->get("Resources"), pageNumber);
                            }
                        }
                    }
                    else if (appearanceStream.isStream() && appearanceStream.getStream()->getDictionary()->hasKey("Resources"))
                    {
                        scanResources(appearanceStream.getStream()->getDictionary()->get("Resources"), pageNumber);
                    }
                }
            }
        }
    }
}

void collectColorants(PDFDocumentSession* session, PDFEvidenceGraph* graph, const PDFEvidenceCollectSettings& settings, PDFProcessingBudget* budget)
{
    PDFColorInventorySettings inventorySettings;
    inventorySettings.probeDpi = settings.colorProbeDpi;
    inventorySettings.richBlackKThreshold = settings.richBlackKThreshold;
    PDFColorInventory inventory(session);
    const PDFColorInventoryResult result = inventory.inspect(inventorySettings);
    int index = 0;
    const auto appendInk = [&](const PDFColorInventoryInk& ink, const QString& target, bool isSpot)
    {
        PDFEvidenceRecord record = makeRecord(graph, PDFEvidenceDomain::Colorants, 1, target);
        record.coverageMethod = QStringLiteral("color-inventory");
        record.fidelity = QStringLiteral("catalog");
        record.objectId = ink.name;
        record.extra.insert(QStringLiteral("name"), ink.name);
        record.extra.insert(QStringLiteral("is_spot"), isSpot);
        record.id = QStringLiteral("colorant:%1").arg(index++);
        appendEvidenceRecord(graph, record, budget);
    };
    for (const PDFColorInventoryInk& ink : result.spotColors)
    {
        appendInk(ink, QStringLiteral("spot-color"), true);
    }
    for (const PDFColorInventoryInk& ink : result.separations)
    {
        appendInk(ink, QStringLiteral("separation"), ink.isSpot);
    }
    for (const PDFRichBlackInventory& richBlack : result.richBlackPages)
    {
        PDFEvidenceRecord record = makeRecord(graph, PDFEvidenceDomain::Colorants, richBlack.page, QStringLiteral("rich-black"));
        record.coverageMethod = QStringLiteral("color-inventory");
        record.fidelity = QStringLiteral("sampled");
        record.observedValue = richBlack.areaMM2;
        record.units = QStringLiteral("mm2");
        record.extra.insert(QStringLiteral("area_mm2"), richBlack.areaMM2);
        record.extra.insert(QStringLiteral("k_threshold"), settings.richBlackKThreshold);
        record.id = QStringLiteral("rich-black:%1").arg(richBlack.page);
        appendEvidenceRecord(graph, record, budget);
    }
}

}   // namespace

PDFEvidenceGraph PDFEvidenceCollector::collect(PDFDocumentSession* session,
                                               PDFEvidenceDomains domains,
                                               const PDFEvidenceCollectSettings& settings)
{
    if (domains == PDFEvidenceDomains())
    {
        domains = pdfEvidenceAllDomains();
    }

    PDFEvidenceGraph graph;
    graph.producerVersion = QString::fromLatin1(PDF_LIBRARY_VERSION);
    if (!session || !session->getDocument())
    {
        graph.complete = false;
        graph.incompleteReason = QStringLiteral("missing-document");
        return graph;
    }

    PDFDocument* document = session->getDocument();
    graph.revision = session->getRevision();

    try
    {
        if (domains.testFlag(PDFEvidenceDomain::Fonts))
        {
            collectFonts(document, &graph, session->getProcessingBudget());
        }
        if (domains.testFlag(PDFEvidenceDomain::Colorants))
        {
            collectColorants(session, &graph, settings, session->getProcessingBudget());
        }

        const bool needsWalk = domains.testFlag(PDFEvidenceDomain::Images) || domains.testFlag(PDFEvidenceDomain::Strokes) || domains.testFlag(PDFEvidenceDomain::OverprintTransparency) || domains.testFlag(PDFEvidenceDomain::Colorants);
        if (needsWalk)
        {
            PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
            PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
            fontCache.setDocument(PDFModifiedDocument(document, &ocActivity));
            fontCache.setCacheShrinkEnabled(nullptr, false);
            PDFCMSManager cmsManager(nullptr);
            cmsManager.setDocument(document);
            PDFCMSPointer cms = cmsManager.getCurrentCMS();
            PDFMeshQualitySettings meshQuality;
            const PDFCatalog* catalog = document->getCatalog();
            for (PDFInteger pageIndex = 0; pageIndex < catalog->getPageCount(); ++pageIndex)
            {
                const PDFPage* page = catalog->getPage(pageIndex);
                if (!page)
                {
                    continue;
                }
                const int pageNumber = int(pageIndex + 1);
                QSet<QString> paintedSpaces;
                if (domains.testFlag(PDFEvidenceDomain::Colorants))
                {
                    std::set<PDFObjectReference> visitedForms;
                    collectColorSpacesFromResources(document, page->getResources(), &paintedSpaces, visitedForms, 0);
                }
                bool foundWhiteOverprint = false;
                QSet<QString> riskyBlendModes;
                QSet<QString> mismatchDescriptions;
                EvidenceProcessor processor(page, document, &fontCache, cms.get(), &ocActivity, meshQuality,
                                            session->getProcessingBudget(), &graph, domains, settings, pageNumber,
                                            &paintedSpaces, &foundWhiteOverprint, &riskyBlendModes, &mismatchDescriptions);
                processor.processContents();
                processAnnotationAppearanceStreams(document, page, [&](const PDFStream* formStream)
                                                   { processor.processFormStream(formStream); });
                if (domains.testFlag(PDFEvidenceDomain::Strokes))
                {
                    throwIfContentProcessingIncomplete(processor.renderErrors());
                }

                if (domains.testFlag(PDFEvidenceDomain::Colorants))
                {
                    QStringList spaces = paintedSpaces.values();
                    spaces.sort();
                    for (const QString& space : spaces)
                    {
                        PDFEvidenceRecord record = makeRecord(&graph, PDFEvidenceDomain::Colorants, pageNumber, QStringLiteral("color-space"));
                        record.coverageMethod = QStringLiteral("content-stream");
                        record.extra.insert(QStringLiteral("space"), space);
                        record.id = QStringLiteral("color-space:%1:%2").arg(pageNumber).arg(space);
                        appendEvidenceRecord(&graph, record, session->getProcessingBudget());
                    }
                }
                if (domains.testFlag(PDFEvidenceDomain::OverprintTransparency) && foundWhiteOverprint)
                {
                    PDFEvidenceRecord record = makeRecord(&graph, PDFEvidenceDomain::OverprintTransparency, pageNumber, QStringLiteral("white-overprint"));
                    record.observedValue = 1;
                    record.id = QStringLiteral("white-overprint:%1").arg(pageNumber);
                    appendEvidenceRecord(&graph, record, session->getProcessingBudget());
                }
                if (domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
                {
                    QStringList blendModes = riskyBlendModes.values();
                    blendModes.sort();
                    if (!blendModes.isEmpty())
                    {
                        PDFEvidenceRecord record = makeRecord(&graph, PDFEvidenceDomain::OverprintTransparency, pageNumber, QStringLiteral("transparency-blend-mode"));
                        QJsonArray names;
                        for (const QString& name : blendModes)
                        {
                            names.append(name);
                        }
                        record.extra.insert(QStringLiteral("blend_modes"), names);
                        record.id = QStringLiteral("transparency-blend-mode:%1").arg(pageNumber);
                        appendEvidenceRecord(&graph, record, session->getProcessingBudget());
                    }
                    QStringList mismatches = mismatchDescriptions.values();
                    mismatches.sort();
                    if (!mismatches.isEmpty())
                    {
                        PDFEvidenceRecord record = makeRecord(&graph, PDFEvidenceDomain::OverprintTransparency, pageNumber, QStringLiteral("transparency-blend-space"));
                        QJsonArray items;
                        for (const QString& item : mismatches)
                        {
                            items.append(item);
                        }
                        record.extra.insert(QStringLiteral("mismatches"), items);
                        record.id = QStringLiteral("transparency-blend-space:%1").arg(pageNumber);
                        appendEvidenceRecord(&graph, record, session->getProcessingBudget());
                    }
                }
            }
        }
    }
    catch (const PDFBudgetExceededException& exception)
    {
        const PDFBudgetExceeded& detail = exception.getDetail();
        graph.complete = false;
        graph.incompleteReason = QString::fromLatin1(getPDFBudgetKindName(detail.kind));
        graph.budgetKind = graph.incompleteReason;
        graph.budgetPool = QString::fromLatin1(getPDFBudgetPoolName(detail.pool));
        graph.budgetLimit = static_cast<qint64>(detail.limit);
        graph.budgetAttempted = static_cast<qint64>(detail.attempted);
        graph.budgetContext = detail.context;
        if (!graph.records.isEmpty())
        {
            graph.records.last().budgetContext = detail.context;
        }
    }
    catch (const PDFException& exception)
    {
        graph.complete = false;
        graph.incompleteReason = exception.getMessage();
    }

    return graph;
}

}   // namespace pdf
