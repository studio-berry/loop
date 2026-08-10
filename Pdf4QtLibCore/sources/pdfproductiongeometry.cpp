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

#include "pdfproductiongeometry.h"
#include "pdfcolorspaces.h"
#include "pdfdocumentsession.h"
#include "pdfoptionalcontent.h"
#include "pdfpagecontentprocessor.h"
#include "pdfdocument.h"

#include <QLineF>
#include <QJsonArray>
#include <QMap>
#include <QPolygonF>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace pdf
{

namespace
{

QString normalizedProcessingStepName(const QString& value)
{
    QString result;
    result.reserve(value.size());
    for (const QChar character : value.toLower())
    {
        if (character.isLetterOrNumber())
        {
            result.append(character);
        }
    }
    return result;
}

PDFProcessingStepType processingStepTypeFromMetadata(const QList<QByteArray>& values)
{
    for (const QByteArray& value : values)
    {
        const QString normalized = normalizedProcessingStepName(QString::fromLatin1(value));
        if (normalized == QStringLiteral("cuttingdie") || normalized == QStringLiteral("cut") || normalized == QStringLiteral("dieline")) return PDFProcessingStepType::CuttingDie;
        if (normalized == QStringLiteral("perforatingcut") || normalized == QStringLiteral("perforation") || normalized == QStringLiteral("perf")) return PDFProcessingStepType::PerforatingCut;
        if (normalized == QStringLiteral("creasingbend") || normalized == QStringLiteral("creasing") || normalized == QStringLiteral("folding") || normalized == QStringLiteral("fold")) return PDFProcessingStepType::CreasingBend;
        if (normalized == QStringLiteral("partialcut") || normalized == QStringLiteral("partial")) return PDFProcessingStepType::PartialCut;
        if (normalized == QStringLiteral("scoringbend") || normalized == QStringLiteral("scoring") || normalized == QStringLiteral("score")) return PDFProcessingStepType::ScoringBend;
        if (normalized == QStringLiteral("foregroundvarnish") || normalized == QStringLiteral("varnish")) return PDFProcessingStepType::ForegroundVarnish;
        if (normalized == QStringLiteral("braille")) return PDFProcessingStepType::Braille;
        if (normalized == QStringLiteral("white") || normalized == QStringLiteral("whiteink")) return PDFProcessingStepType::White;
        if (normalized == QStringLiteral("legend")) return PDFProcessingStepType::Legend;
        if (normalized == QStringLiteral("positions")) return PDFProcessingStepType::Positions;
        if (normalized == QStringLiteral("positionsunspecified")) return PDFProcessingStepType::PositionsUnspecified;
    }
    return PDFProcessingStepType::Unknown;
}

PDFProcessingStepKind productionKindForType(PDFProcessingStepType type)
{
    switch (type)
    {
        case PDFProcessingStepType::CuttingDie:
        case PDFProcessingStepType::PartialCut: return PDFProcessingStepKind::Cut;
        case PDFProcessingStepType::PerforatingCut: return PDFProcessingStepKind::Perforation;
        case PDFProcessingStepType::CreasingBend:
        case PDFProcessingStepType::ScoringBend: return PDFProcessingStepKind::Crease;
        case PDFProcessingStepType::ForegroundVarnish: return PDFProcessingStepKind::Varnish;
        case PDFProcessingStepType::White: return PDFProcessingStepKind::WhiteInk;
        case PDFProcessingStepType::Positions: return PDFProcessingStepKind::Registration;
        case PDFProcessingStepType::Braille:
        case PDFProcessingStepType::Legend:
        case PDFProcessingStepType::PositionsUnspecified:
        case PDFProcessingStepType::Unknown: return PDFProcessingStepKind::TechnicalNonPrinting;
    }
    return PDFProcessingStepKind::Custom;
}

struct ProcessingStepGeometry
{
    PDFObjectReference reference;
    QPainterPath geometry;
    QVector<int> pages;
};

bool validReference(const PDFObjectReference& reference)
{
    return reference.objectNumber > 0;
}

class ProcessingStepCollector final : public PDFPageContentProcessor
{
public:
    ProcessingStepCollector(const PDFPage* page,
                            const PDFDocument* document,
                            const PDFFontCache* fontCache,
                            const PDFCMS* cms,
                            int pageIndex,
                            QVector<ProcessingStepGeometry>* ocgGeometry,
                            QMap<QString, QPainterPath>* legacyGeometry,
                            QMap<QString, QVector<int>>* legacyPages) :
        PDFPageContentProcessor(page, document, fontCache, cms, nullptr, QTransform(), {}, nullptr),
        m_pageIndex(pageIndex),
        m_ocgGeometry(ocgGeometry),
        m_legacyGeometry(legacyGeometry),
        m_legacyPages(legacyPages)
    {
    }

protected:
    void performMarkedContentBegin(const QByteArray& tag, const PDFObject& properties) override
    {
        m_ocgStack.append(m_currentOcg);
        if (tag == "OC")
        {
            PDFObject resolved = properties;
            if (resolved.isName() && getPropertiesDictionary())
            {
                resolved = getPropertiesDictionary()->get(resolved.getString());
            }
            resolved = getDocument()->getObject(resolved);
            if (resolved.isDictionary())
            {
                resolved = resolved.getDictionary()->get("OC");
                resolved = getDocument()->getObject(resolved);
            }
            m_currentOcg = resolved.isReference() ? resolved.getReference() : PDFObjectReference();
        }
    }

    void performMarkedContentEnd() override
    {
        if (!m_ocgStack.isEmpty())
        {
            m_currentOcg = m_ocgStack.takeLast();
        }
        else
        {
            m_currentOcg = PDFObjectReference();
        }
    }

    void performBeforePathPainting(const QPainterPath& path,
                                   bool stroke,
                                   bool fill,
                                   bool text,
                                   Qt::FillRule fillRule) override
    {
        Q_UNUSED(stroke);
        Q_UNUSED(fill);
        Q_UNUSED(text);
        Q_UNUSED(fillRule);
        const QPainterPath pagePath = getGraphicState()->getCurrentTransformationMatrix().map(path);
        if (validReference(m_currentOcg))
        {
            auto it = std::find_if(m_ocgGeometry->begin(), m_ocgGeometry->end(), [this](const ProcessingStepGeometry& value)
            {
                return value.reference == m_currentOcg;
            });
            if (it == m_ocgGeometry->end())
            {
                m_ocgGeometry->append({ m_currentOcg, pagePath, { m_pageIndex } });
            }
            else
            {
                it->geometry.addPath(pagePath);
                if (!it->pages.contains(m_pageIndex))
                {
                    it->pages.append(m_pageIndex);
                }
            }
            return;
        }

        const PDFAbstractColorSpace* colorSpaces[] = {
            getGraphicState()->getStrokeColorSpace(),
            getGraphicState()->getFillColorSpace()
        };
        for (const PDFAbstractColorSpace* colorSpace : colorSpaces)
        {
            if (!colorSpace || colorSpace->getColorSpace() != PDFAbstractColorSpace::ColorSpace::Separation)
            {
                continue;
            }
            const auto* separation = static_cast<const PDFSeparationColorSpace*>(colorSpace);
            const QString colorName = QString::fromLatin1(separation->getColorName());
            const QString normalized = normalizedProcessingStepName(colorName);
            if (normalized != QStringLiteral("cutcontour") && normalized != QStringLiteral("die") &&
                normalized != QStringLiteral("dieline") && normalized != QStringLiteral("thrucut"))
            {
                continue;
            }
            m_legacyGeometry->operator[](colorName).addPath(pagePath);
            QVector<int>& pages = m_legacyPages->operator[](colorName);
            if (!pages.contains(m_pageIndex))
            {
                pages.append(m_pageIndex);
            }
        }
    }

private:
    int m_pageIndex;
    PDFObjectReference m_currentOcg;
    QVector<PDFObjectReference> m_ocgStack;
    QVector<ProcessingStepGeometry>* m_ocgGeometry;
    QMap<QString, QPainterPath>* m_legacyGeometry;
    QMap<QString, QVector<int>>* m_legacyPages;
};

QString severityToString(PDFProductionDiagnosticSeverity severity)
{
    switch (severity)
    {
        case PDFProductionDiagnosticSeverity::Info: return QStringLiteral("info");
        case PDFProductionDiagnosticSeverity::Warning: return QStringLiteral("warning");
        case PDFProductionDiagnosticSeverity::Error: return QStringLiteral("error");
    }
    return QStringLiteral("warning");
}

PDFProductionDiagnosticSeverity severityFromString(const QString& value)
{
    if (value.trimmed().compare(QStringLiteral("error"), Qt::CaseInsensitive) == 0)
    {
        return PDFProductionDiagnosticSeverity::Error;
    }
    if (value.trimmed().compare(QStringLiteral("info"), Qt::CaseInsensitive) == 0)
    {
        return PDFProductionDiagnosticSeverity::Info;
    }
    return PDFProductionDiagnosticSeverity::Warning;
}

QJsonObject pathToJson(const QPainterPath& path)
{
    QJsonArray elements;
    for (int index = 0; index < path.elementCount(); ++index)
    {
        const QPainterPath::Element element = path.elementAt(index);
        elements.append(QJsonObject{
            { QStringLiteral("x"), element.x },
            { QStringLiteral("y"), element.y },
            { QStringLiteral("type"), int(element.type) }
        });
    }
    return QJsonObject{
        { QStringLiteral("fillRule"), int(path.fillRule()) },
        { QStringLiteral("elements"), elements }
    };
}

QPainterPath pathFromJson(const QJsonObject& object)
{
    QPainterPath path;
    path.setFillRule(Qt::FillRule(object.value(QStringLiteral("fillRule")).toInt(int(Qt::WindingFill))));
    const QJsonArray elements = object.value(QStringLiteral("elements")).toArray();
    for (int index = 0; index < elements.size(); ++index)
    {
        const QJsonObject element = elements.at(index).toObject();
        const QPointF point(element.value(QStringLiteral("x")).toDouble(),
                             element.value(QStringLiteral("y")).toDouble());
        const QPainterPath::ElementType type = QPainterPath::ElementType(element.value(QStringLiteral("type")).toInt(1));
        if (type == QPainterPath::MoveToElement)
        {
            path.moveTo(point);
        }
        else if (type == QPainterPath::LineToElement)
        {
            path.lineTo(point);
        }
        else if (type == QPainterPath::CurveToElement && index + 2 < elements.size())
        {
            const QJsonObject control1 = elements.at(index + 1).toObject();
            const QJsonObject control2 = elements.at(index + 2).toObject();
            path.cubicTo(point,
                         QPointF(control1.value(QStringLiteral("x")).toDouble(), control1.value(QStringLiteral("y")).toDouble()),
                         QPointF(control2.value(QStringLiteral("x")).toDouble(), control2.value(QStringLiteral("y")).toDouble()));
            index += 2;
        }
    }
    return path;
}

QJsonArray diagnosticsToJson(const QVector<PDFProductionDiagnostic>& diagnostics)
{
    QJsonArray result;
    for (const PDFProductionDiagnostic& diagnostic : diagnostics)
    {
        result.append(diagnostic.toJson());
    }
    return result;
}

void addDiagnostic(QVector<PDFProductionDiagnostic>& diagnostics,
                   const QString& id,
                   PDFProductionDiagnosticSeverity severity,
                   const QString& message,
                   int pageIndex = -1,
                   const QString& objectId = {})
{
    diagnostics.append({ id, severity, message, pageIndex, objectId });
}

QList<QPolygonF> contourPolygons(const QPainterPath& path)
{
    return path.toSubpathPolygons(QTransform());
}

int polygonSegmentCount(const QPolygonF& polygon)
{
    return polygon.size() >= 2 ? polygon.size() : 0;
}

bool pointsNear(const QPointF& first, const QPointF& second, double tolerance)
{
    return QLineF(first, second).length() <= tolerance;
}

bool hasExplicitlyClosedSubpaths(const QPainterPath& path, double tolerance)
{
    bool haveSubpath = false;
    QPointF first;
    QPointF last;
    for (int index = 0; index < path.elementCount(); ++index)
    {
        const QPainterPath::Element element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        if (element.type == QPainterPath::MoveToElement)
        {
            if (haveSubpath && !pointsNear(first, last, tolerance))
            {
                return false;
            }
            first = point;
            last = point;
            haveSubpath = true;
        }
        else
        {
            last = point;
        }
    }
    return haveSubpath && pointsNear(first, last, tolerance);
}

bool segmentsCross(const QLineF& first, const QLineF& second, double tolerance)
{
    if (pointsNear(first.p1(), second.p1(), tolerance) ||
        pointsNear(first.p1(), second.p2(), tolerance) ||
        pointsNear(first.p2(), second.p1(), tolerance) ||
        pointsNear(first.p2(), second.p2(), tolerance))
    {
        return false;
    }
    QPointF intersection;
    return first.intersects(second, &intersection) == QLineF::BoundedIntersection;
}

QPainterPath normalizedContour(const PDFProductionContour& contour)
{
    QPainterPath normalized;
    normalized.setFillRule(Qt::WindingFill);
    const double tolerance = qMax(0.000001, contour.flatteningTolerancePt);
    for (const QPolygonF& sourcePolygon : contourPolygons(contour.path))
    {
        QPolygonF polygon;
        for (const QPointF& point : sourcePolygon)
        {
            if (polygon.isEmpty() || !pointsNear(polygon.back(), point, tolerance))
            {
                polygon.append(point);
            }
        }
        if (polygon.size() > 1 && pointsNear(polygon.front(), polygon.back(), tolerance))
        {
            polygon.removeLast();
        }
        if (polygon.size() < 3)
        {
            continue;
        }
        normalized.moveTo(polygon.front());
        for (int index = 1; index < polygon.size(); ++index)
        {
            normalized.lineTo(polygon.at(index));
        }
        normalized.closeSubpath();
    }
    return normalized;
}

void validateContour(const PDFProductionContour& contour,
                     const PDFProductionValidationOptions& options,
                     PDFProductionValidationReport* report)
{
    const QList<QPolygonF> polygons = contourPolygons(contour.path);
    int contourSegments = 0;
    if (contour.id.trimmed().isEmpty())
    {
        addDiagnostic(report->diagnostics, QStringLiteral("production.contour.missing_id"),
                      PDFProductionDiagnosticSeverity::Error, QStringLiteral("Contour id is required."), contour.pageIndex);
    }
    if (contour.path.isEmpty() || polygons.isEmpty())
    {
        addDiagnostic(report->diagnostics, QStringLiteral("production.contour.missing"),
                      PDFProductionDiagnosticSeverity::Error, QStringLiteral("Contour has no geometry."), contour.pageIndex, contour.id);
        return;
    }
    if (!contour.closed || !hasExplicitlyClosedSubpaths(contour.path, options.tolerancePt))
    {
        addDiagnostic(report->diagnostics, QStringLiteral("production.contour.open"),
                      PDFProductionDiagnosticSeverity::Error, QStringLiteral("Contour must be explicitly closed."), contour.pageIndex, contour.id);
    }
    if (!(contour.flatteningTolerancePt > 0.0) || !std::isfinite(contour.flatteningTolerancePt))
    {
        addDiagnostic(report->diagnostics, QStringLiteral("production.contour.invalid_tolerance"),
                      PDFProductionDiagnosticSeverity::Error, QStringLiteral("Contour flattening tolerance must be finite and positive."), contour.pageIndex, contour.id);
    }

    for (const QPolygonF& sourcePolygon : polygons)
    {
        QPolygonF polygon = sourcePolygon;
        if (polygon.size() > 1 && pointsNear(polygon.front(), polygon.back(), options.tolerancePt))
        {
            polygon.removeLast();
        }
        contourSegments += polygonSegmentCount(polygon);
        const int segmentCount = polygon.size();
        for (int first = 0; first < segmentCount; ++first)
        {
            const QLineF firstSegment(polygon.at(first), polygon.at((first + 1) % segmentCount));
            if (firstSegment.length() <= options.tolerancePt)
            {
                addDiagnostic(report->diagnostics, QStringLiteral("production.contour.zero_length_segment"),
                              PDFProductionDiagnosticSeverity::Error, QStringLiteral("Contour contains a zero-length segment."), contour.pageIndex, contour.id);
            }
            for (int second = first + 1; second < segmentCount; ++second)
            {
                if (second == first + 1 || (first == 0 && second == segmentCount - 1))
                {
                    continue;
                }
                if (segmentsCross(firstSegment,
                                  QLineF(polygon.at(second), polygon.at((second + 1) % segmentCount)),
                                  options.tolerancePt))
                {
                    addDiagnostic(report->diagnostics, QStringLiteral("production.contour.self_intersection"),
                                  PDFProductionDiagnosticSeverity::Error, QStringLiteral("Contour contains a self-intersection."), contour.pageIndex, contour.id);
                    first = segmentCount;
                    break;
                }
            }
        }
    }
    report->segmentCount += contourSegments;
    if (report->segmentCount > options.maxSegments)
    {
        addDiagnostic(report->diagnostics, QStringLiteral("production.contour.complexity_limit"),
                      PDFProductionDiagnosticSeverity::Error, QStringLiteral("Production geometry exceeds the segment complexity budget."), contour.pageIndex, contour.id);
    }
}

QPointF pointAlong(const QLineF& line, double distance)
{
    const double length = line.length();
    if (length <= 0.0)
    {
        return line.p1();
    }
    return line.pointAt(qBound(0.0, distance / length, 1.0));
}

void appendEdgePoints(QVector<QPointF>& points,
                      const QLineF& edge,
                      const PDFGrommetSpec& spec,
                      bool forward)
{
    const double length = edge.length();
    const double offset = qMax(spec.edgeOffsetPt, spec.safeAreaInsetPt + spec.diameterPt * 0.5);
    const double usable = length - 2.0 * offset;
    if (usable <= 0.0)
    {
        return;
    }
    const int intervals = qMax(1, int(std::ceil(usable / spec.targetSpacingPt)));
    const int count = spec.includeCorners ? intervals + 1 : intervals;
    for (int index = 0; index < count; ++index)
    {
        const double fraction = spec.includeCorners
                ? double(index) / double(intervals)
                : double(index + 1) / double(intervals + 1);
        const double distance = offset + usable * fraction;
        const QPointF point = pointAlong(edge, distance);
        bool duplicate = false;
        for (const QPointF& existing : points)
        {
            if (pointsNear(existing, point, 0.000001))
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }
        if (forward)
        {
            points.append(point);
        }
        else
        {
            points.prepend(point);
        }
    }
}

} // namespace

QString pdfProcessingStepKindToString(PDFProcessingStepKind kind)
{
    switch (kind)
    {
        case PDFProcessingStepKind::Cut: return QStringLiteral("cut");
        case PDFProcessingStepKind::Crease: return QStringLiteral("crease");
        case PDFProcessingStepKind::Perforation: return QStringLiteral("perforation");
        case PDFProcessingStepKind::WhiteInk: return QStringLiteral("white-ink");
        case PDFProcessingStepKind::Varnish: return QStringLiteral("varnish");
        case PDFProcessingStepKind::Registration: return QStringLiteral("registration");
        case PDFProcessingStepKind::TechnicalNonPrinting: return QStringLiteral("technical-non-printing");
        case PDFProcessingStepKind::Custom: return QStringLiteral("custom");
    }
    return QStringLiteral("custom");
}

PDFProcessingStepKind pdfProcessingStepKindFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("cut") || normalized == QStringLiteral("cutcontour") || normalized == QStringLiteral("kisscut")) return PDFProcessingStepKind::Cut;
    if (normalized == QStringLiteral("crease") || normalized == QStringLiteral("fold")) return PDFProcessingStepKind::Crease;
    if (normalized == QStringLiteral("perforation") || normalized == QStringLiteral("perf")) return PDFProcessingStepKind::Perforation;
    if (normalized == QStringLiteral("white-ink") || normalized == QStringLiteral("whiteink")) return PDFProcessingStepKind::WhiteInk;
    if (normalized == QStringLiteral("varnish") || normalized == QStringLiteral("spot-coating")) return PDFProcessingStepKind::Varnish;
    if (normalized == QStringLiteral("registration")) return PDFProcessingStepKind::Registration;
    if (normalized == QStringLiteral("technical-non-printing") || normalized == QStringLiteral("technical")) return PDFProcessingStepKind::TechnicalNonPrinting;
    return PDFProcessingStepKind::Custom;
}

QString pdfProcessingStepTypeToString(PDFProcessingStepType type)
{
    switch (type)
    {
        case PDFProcessingStepType::CuttingDie: return QStringLiteral("cutting-die");
        case PDFProcessingStepType::PerforatingCut: return QStringLiteral("perforating-cut");
        case PDFProcessingStepType::CreasingBend: return QStringLiteral("creasing-bend");
        case PDFProcessingStepType::PartialCut: return QStringLiteral("partial-cut");
        case PDFProcessingStepType::ScoringBend: return QStringLiteral("scoring-bend");
        case PDFProcessingStepType::ForegroundVarnish: return QStringLiteral("foreground-varnish");
        case PDFProcessingStepType::Braille: return QStringLiteral("braille");
        case PDFProcessingStepType::White: return QStringLiteral("white");
        case PDFProcessingStepType::Legend: return QStringLiteral("legend");
        case PDFProcessingStepType::Positions: return QStringLiteral("positions");
        case PDFProcessingStepType::PositionsUnspecified: return QStringLiteral("positions-unspecified");
        case PDFProcessingStepType::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

PDFProcessingStepType pdfProcessingStepTypeFromString(const QString& value)
{
    return processingStepTypeFromMetadata({ value.toLatin1() });
}

QJsonObject PDFProductionDiagnostic::toJson() const
{
    QJsonObject object{{ QStringLiteral("id"), id },
                        { QStringLiteral("severity"), severityToString(severity) },
                        { QStringLiteral("message"), message }};
    if (pageIndex >= 0) object.insert(QStringLiteral("page"), pageIndex + 1);
    if (!objectId.isEmpty()) object.insert(QStringLiteral("objectId"), objectId);
    return object;
}

QJsonObject PDFProductionContour::toJson() const
{
    return QJsonObject{{ QStringLiteral("id"), id },
                        { QStringLiteral("page"), pageIndex + 1 },
                        { QStringLiteral("path"), pathToJson(path) },
                        { QStringLiteral("closed"), closed },
                        { QStringLiteral("hole"), hole },
                        { QStringLiteral("flatteningTolerancePt"), flatteningTolerancePt },
                        { QStringLiteral("sourceObjectRef"), sourceObjectRef },
                        { QStringLiteral("sourceEvidence"), sourceEvidence }};
}

PDFProductionContour PDFProductionContour::fromJson(const QJsonObject& object)
{
    PDFProductionContour contour;
    contour.id = object.value(QStringLiteral("id")).toString();
    contour.pageIndex = object.value(QStringLiteral("page")).toInt(1) - 1;
    contour.path = pathFromJson(object.value(QStringLiteral("path")).toObject());
    contour.closed = object.value(QStringLiteral("closed")).toBool(true);
    contour.hole = object.value(QStringLiteral("hole")).toBool(false);
    contour.flatteningTolerancePt = object.value(QStringLiteral("flatteningTolerancePt")).toDouble(0.1);
    contour.sourceObjectRef = object.value(QStringLiteral("sourceObjectRef")).toString();
    contour.sourceEvidence = object.value(QStringLiteral("sourceEvidence")).toString();
    return contour;
}

QJsonObject PDFProcessingStep::toJson() const
{
    QJsonObject object{{ QStringLiteral("id"), id },
                        { QStringLiteral("kind"), pdfProcessingStepKindToString(kind) },
                        { QStringLiteral("displayName"), displayName },
                        { QStringLiteral("spotColorName"), spotColorName },
                        { QStringLiteral("shouldPrint"), shouldPrint },
                        { QStringLiteral("overprint"), overprint },
                        { QStringLiteral("vendorMetadata"), QJsonObject::fromVariantMap(vendorMetadata) }};
    if (type != PDFProcessingStepType::Unknown)
    {
        object.insert(QStringLiteral("type"), pdfProcessingStepTypeToString(type));
    }
    if (!ocgName.isEmpty())
    {
        object.insert(QStringLiteral("ocgName"), ocgName);
    }
    if (validReference(ocg))
    {
        object.insert(QStringLiteral("ocgObject"), qint64(ocg.objectNumber));
        object.insert(QStringLiteral("ocgGeneration"), qint64(ocg.generation));
    }
    if (!geometry.isEmpty())
    {
        object.insert(QStringLiteral("geometry"), pathToJson(geometry));
    }
    if (isSeparation)
    {
        object.insert(QStringLiteral("isSeparation"), true);
    }
    if (!detectionMethod.isEmpty())
    {
        object.insert(QStringLiteral("detectionMethod"), detectionMethod);
    }
    if (!pageIndices.isEmpty())
    {
        QJsonArray pages;
        for (const int pageIndex : pageIndices) pages.append(pageIndex + 1);
        object.insert(QStringLiteral("pages"), pages);
    }
    return object;
}

PDFProcessingStep PDFProcessingStep::fromJson(const QJsonObject& object)
{
    PDFProcessingStep step;
    step.id = object.value(QStringLiteral("id")).toString();
    step.kind = pdfProcessingStepKindFromString(object.value(QStringLiteral("kind")).toString());
    step.displayName = object.value(QStringLiteral("displayName")).toString();
    step.spotColorName = object.value(QStringLiteral("spotColorName")).toString();
    step.shouldPrint = object.value(QStringLiteral("shouldPrint")).toBool(true);
    step.overprint = object.value(QStringLiteral("overprint")).toBool(false);
    step.vendorMetadata = object.value(QStringLiteral("vendorMetadata")).toObject().toVariantMap();
    step.type = pdfProcessingStepTypeFromString(object.value(QStringLiteral("type")).toString());
    step.ocgName = object.value(QStringLiteral("ocgName")).toString();
    step.ocg = PDFObjectReference(object.value(QStringLiteral("ocgObject")).toInteger(),
                                  object.value(QStringLiteral("ocgGeneration")).toInteger());
    step.geometry = pathFromJson(object.value(QStringLiteral("geometry")).toObject());
    step.isSeparation = object.value(QStringLiteral("isSeparation")).toBool(false);
    step.detectionMethod = object.value(QStringLiteral("detectionMethod")).toString();
    for (const QJsonValue& page : object.value(QStringLiteral("pages")).toArray())
    {
        step.pageIndices.append(page.toInt(1) - 1);
    }
    return step;
}

QJsonObject PDFProductionPath::toJson() const
{
    return QJsonObject{{ QStringLiteral("id"), id },
                        { QStringLiteral("geometry"), pathToJson(geometry) },
                        { QStringLiteral("processingStepId"), processingStepId },
                        { QStringLiteral("sourceEvidence"), sourceEvidence }};
}

PDFProductionPath PDFProductionPath::fromJson(const QJsonObject& object)
{
    PDFProductionPath path;
    path.id = object.value(QStringLiteral("id")).toString();
    path.geometry = pathFromJson(object.value(QStringLiteral("geometry")).toObject());
    path.processingStepId = object.value(QStringLiteral("processingStepId")).toString();
    path.sourceEvidence = object.value(QStringLiteral("sourceEvidence")).toString();
    return path;
}

QJsonObject PDFProductionGeometryModel::toJson() const
{
    QJsonArray contoursJson;
    for (const PDFProductionContour& contour : contours) contoursJson.append(contour.toJson());
    QJsonArray stepsJson;
    for (const PDFProcessingStep& step : processingSteps) stepsJson.append(step.toJson());
    QJsonArray pathsJson;
    for (const PDFProductionPath& path : processingPaths) pathsJson.append(path.toJson());
    return QJsonObject{{ QStringLiteral("schema"), QStringLiteral("loupe-production-geometry/%1").arg(schemaVersion) },
                        { QStringLiteral("schemaVersion"), schemaVersion },
                        { QStringLiteral("contours"), contoursJson },
                        { QStringLiteral("processingSteps"), stepsJson },
                        { QStringLiteral("processingPaths"), pathsJson },
                        { QStringLiteral("vendorMetadata"), QJsonObject::fromVariantMap(vendorMetadata) }};
}

PDFProductionGeometryModel PDFProductionGeometryModel::fromJson(const QJsonObject& object)
{
    PDFProductionGeometryModel model;
    model.schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt(PDFProductionGeometrySchemaVersion);
    for (const QJsonValue& value : object.value(QStringLiteral("contours")).toArray()) model.contours.append(PDFProductionContour::fromJson(value.toObject()));
    for (const QJsonValue& value : object.value(QStringLiteral("processingSteps")).toArray()) model.processingSteps.append(PDFProcessingStep::fromJson(value.toObject()));
    for (const QJsonValue& value : object.value(QStringLiteral("processingPaths")).toArray()) model.processingPaths.append(PDFProductionPath::fromJson(value.toObject()));
    model.vendorMetadata = object.value(QStringLiteral("vendorMetadata")).toObject().toVariantMap();
    return model;
}

QList<PDFProcessingStep> detectProcessingSteps(const PDFDocument& document)
{
    QList<PDFProcessingStep> result;
    const PDFOptionalContentProperties* properties = document.getCatalog()->getOptionalContentProperties();
    if (!properties)
    {
        return result;
    }

    struct ClassifiedGroup
    {
        PDFOptionalContentGroup group;
        PDFProcessingStepType type = PDFProcessingStepType::Unknown;
    };
    QVector<ClassifiedGroup> groups;
    PDFDocumentDataLoaderDecorator loader(&document);
    for (const PDFObjectReference reference : properties->getAllOptionalContentGroups())
    {
        if (!properties->hasOptionalContentGroup(reference))
        {
            continue;
        }
        const PDFOptionalContentGroup& group = properties->getOptionalContentGroup(reference);
        QList<QByteArray> metadata;
        if (!group.getUsageType().isEmpty()) metadata.append(group.getUsageType());
        if (!group.getSubtype().isEmpty()) metadata.append(group.getSubtype());
        const QByteArray pageElement = loader.readName(group.getPageElement());
        if (!pageElement.isEmpty()) metadata.append(pageElement);
        const PDFProcessingStepType type = processingStepTypeFromMetadata(metadata);
        const bool hasProcessingMetadata = !metadata.isEmpty() &&
                                           !std::all_of(metadata.cbegin(), metadata.cend(), [](const QByteArray& value)
        {
            return value.compare("OCG", Qt::CaseInsensitive) == 0;
        });
        if (type != PDFProcessingStepType::Unknown || hasProcessingMetadata)
        {
            groups.append({ group, type });
        }
    }

    QVector<ProcessingStepGeometry> ocgGeometry;
    QMap<QString, QPainterPath> legacyGeometry;
    QMap<QString, QVector<int>> legacyPages;
    PDFDocumentSession session(&document);
    for (size_t pageIndex = 0; pageIndex < document.getCatalog()->getPageCount(); ++pageIndex)
    {
        const PDFPage* page = document.getCatalog()->getPage(pageIndex);
        try
        {
            ProcessingStepCollector collector(page, &document, session.getFontCache(), session.getCMS(),
                                               int(pageIndex), &ocgGeometry, &legacyGeometry, &legacyPages);
            collector.processContents();
        }
        catch (const std::exception&)
        {
            // A malformed page must not hide valid OCG metadata or legacy
            // color evidence from the remaining pages.
        }
    }

    for (const ClassifiedGroup& classified : groups)
    {
        PDFProcessingStep step;
        step.id = QStringLiteral("ocg-%1-%2").arg(classified.group.getReference().objectNumber)
                                              .arg(classified.group.getReference().generation);
        step.kind = productionKindForType(classified.type);
        step.type = classified.type;
        step.displayName = classified.group.getName();
        step.ocgName = classified.group.getName();
        step.ocg = classified.group.getReference();
        step.shouldPrint = classified.group.getUsagePrintState() != OCState::OFF;
        step.detectionMethod = QStringLiteral("iso-19593-1");
        step.vendorMetadata.insert(QStringLiteral("usageType"), QString::fromLatin1(classified.group.getUsageType()));
        step.vendorMetadata.insert(QStringLiteral("creator"), classified.group.getCreator());
        step.vendorMetadata.insert(QStringLiteral("subtype"), QString::fromLatin1(classified.group.getSubtype()));
        for (const ProcessingStepGeometry& geometry : ocgGeometry)
        {
            if (geometry.reference == step.ocg)
            {
                step.geometry = geometry.geometry;
                step.pageIndices = geometry.pages;
                break;
            }
        }
        result.append(step);
    }

    for (auto it = legacyGeometry.cbegin(); it != legacyGeometry.cend(); ++it)
    {
        PDFProcessingStep step;
        step.id = QStringLiteral("spot-%1").arg(normalizedProcessingStepName(it.key()));
        step.kind = PDFProcessingStepKind::Cut;
        step.type = PDFProcessingStepType::CuttingDie;
        step.displayName = it.key();
        step.spotColorName = it.key();
        step.geometry = it.value();
        step.pageIndices = legacyPages.value(it.key());
        step.shouldPrint = false;
        step.isSeparation = true;
        step.detectionMethod = QStringLiteral("legacy-spot-color");
        step.vendorMetadata.insert(QStringLiteral("legacyAliases"), QStringList{
            QStringLiteral("CutContour"), QStringLiteral("Die"),
            QStringLiteral("Dieline"), QStringLiteral("Thru-cut") });
        result.append(step);
    }

    return result;
}

QString PDFProcessingStepRegistry::canonicalKindForAlias(const QString& alias)
{
    return pdfProcessingStepKindToString(pdfProcessingStepKindFromString(alias));
}

bool PDFProcessingStepRegistry::isKnownAlias(const QString& alias)
{
    const QString value = alias.trimmed().toLower();
    return value == QStringLiteral("cut") || value == QStringLiteral("cutcontour") || value == QStringLiteral("kisscut") ||
           value == QStringLiteral("crease") || value == QStringLiteral("fold") || value == QStringLiteral("perforation") ||
           value == QStringLiteral("perf") || value == QStringLiteral("white-ink") || value == QStringLiteral("whiteink") ||
           value == QStringLiteral("varnish") || value == QStringLiteral("spot-coating") || value == QStringLiteral("registration") ||
           value == QStringLiteral("technical-non-printing") || value == QStringLiteral("technical");
}

PDFProductionValidationReport validateProductionGeometry(const PDFProductionGeometryModel& model,
                                                         const PDFProductionValidationOptions& options)
{
    PDFProductionValidationReport report;
    if (model.schemaVersion != PDFProductionGeometrySchemaVersion)
    {
        addDiagnostic(report.diagnostics, QStringLiteral("production.schema.unsupported"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Production geometry schema version is not supported."));
    }
    if (model.contours.size() > options.maxContours)
    {
        addDiagnostic(report.diagnostics, QStringLiteral("production.contour.count_limit"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Production geometry contains too many contours."));
    }
    QSet<QString> stepIds;
    for (const PDFProcessingStep& step : model.processingSteps)
    {
        if (step.id.isEmpty() || stepIds.contains(step.id))
        {
            addDiagnostic(report.diagnostics, QStringLiteral("production.processing_step.duplicate"), PDFProductionDiagnosticSeverity::Error,
                          QStringLiteral("Processing step ids must be non-empty and unique."), -1, step.id);
        }
        stepIds.insert(step.id);
    }
    for (const PDFProductionContour& contour : model.contours)
    {
        validateContour(contour, options, &report);
    }
    for (const PDFProductionPath& path : model.processingPaths)
    {
        if (!stepIds.contains(path.processingStepId))
        {
            addDiagnostic(report.diagnostics, QStringLiteral("production.processing_step.missing"), PDFProductionDiagnosticSeverity::Error,
                          QStringLiteral("Processing path references an unknown typed processing step."), -1, path.id);
        }
    }
    report.valid = std::none_of(report.diagnostics.cbegin(), report.diagnostics.cend(), [](const PDFProductionDiagnostic& diagnostic)
    {
        return diagnostic.severity == PDFProductionDiagnosticSeverity::Error;
    });
    return report;
}

QJsonObject PDFProductionValidationReport::toJson() const
{
    return QJsonObject{{ QStringLiteral("valid"), valid },
                        { QStringLiteral("segmentCount"), segmentCount },
                        { QStringLiteral("diagnostics"), diagnosticsToJson(diagnostics) }};
}

PDFContourBleedPlan planContourBleed(const PDFProductionContour& contour,
                                     const PDFContourBleedSettings& settings)
{
    PDFContourBleedPlan plan;
    plan.contourId = contour.id;
    plan.sourceBounds = contour.path.boundingRect();
    if (!contour.closed)
    {
        addDiagnostic(plan.diagnostics, QStringLiteral("production.contour.open"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Contour bleed requires a closed contour."), contour.pageIndex, contour.id);
        return plan;
    }
    if (!(settings.amountPt > 0.0) || !std::isfinite(settings.amountPt))
    {
        addDiagnostic(plan.diagnostics, QStringLiteral("production.contour_bleed.invalid_amount"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Contour bleed amount must be finite and positive."), contour.pageIndex, contour.id);
        return plan;
    }
    PDFProductionValidationOptions validationOptions;
    validationOptions.tolerancePt = settings.flatteningTolerancePt;
    validationOptions.maxSegments = settings.maxSegments;
    PDFProductionGeometryModel model;
    model.contours.append(contour);
    const PDFProductionValidationReport validation = validateProductionGeometry(model, validationOptions);
    plan.segmentCount = validation.segmentCount;
    plan.diagnostics = validation.diagnostics;
    if (!validation.valid)
    {
        return plan;
    }

    const QPainterPath normalized = normalizedContour(contour);
    QPainterPathStroker stroker;
    stroker.setWidth(settings.amountPt * 2.0);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);
    plan.annularRing = stroker.createStroke(normalized).subtracted(normalized);
    plan.annularRing.setFillRule(Qt::WindingFill);
    plan.bleedBounds = plan.annularRing.boundingRect();
    plan.valid = !plan.annularRing.isEmpty() && plan.bleedBounds.isValid();
    if (!plan.valid)
    {
        addDiagnostic(plan.diagnostics, QStringLiteral("production.contour_bleed.empty"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Contour bleed produced an empty ring."), contour.pageIndex, contour.id);
    }
    return plan;
}

QJsonObject PDFContourBleedPlan::toJson() const
{
    return QJsonObject{{ QStringLiteral("valid"), valid },
                        { QStringLiteral("contourId"), contourId },
                        { QStringLiteral("sourceBounds"), QJsonObject{{ QStringLiteral("x"), sourceBounds.x() }, { QStringLiteral("y"), sourceBounds.y() }, { QStringLiteral("width"), sourceBounds.width() }, { QStringLiteral("height"), sourceBounds.height() }} },
                        { QStringLiteral("bleedBounds"), QJsonObject{{ QStringLiteral("x"), bleedBounds.x() }, { QStringLiteral("y"), bleedBounds.y() }, { QStringLiteral("width"), bleedBounds.width() }, { QStringLiteral("height"), bleedBounds.height() }} },
                        { QStringLiteral("segmentCount"), segmentCount },
                        { QStringLiteral("diagnostics"), diagnosticsToJson(diagnostics) }};
}

QJsonObject PDFGrommetSpec::toJson() const
{
    return QJsonObject{{ QStringLiteral("diameterPt"), diameterPt },
                        { QStringLiteral("edgeOffsetPt"), edgeOffsetPt },
                        { QStringLiteral("targetSpacingPt"), targetSpacingPt },
                        { QStringLiteral("minimumSpacingPt"), minimumSpacingPt },
                        { QStringLiteral("safeAreaInsetPt"), safeAreaInsetPt },
                        { QStringLiteral("includeCorners"), includeCorners }};
}

PDFGrommetSpec PDFGrommetSpec::fromJson(const QJsonObject& object)
{
    PDFGrommetSpec spec;
    spec.diameterPt = object.value(QStringLiteral("diameterPt")).toDouble(spec.diameterPt);
    spec.edgeOffsetPt = object.value(QStringLiteral("edgeOffsetPt")).toDouble(spec.edgeOffsetPt);
    spec.targetSpacingPt = object.value(QStringLiteral("targetSpacingPt")).toDouble(spec.targetSpacingPt);
    spec.minimumSpacingPt = object.value(QStringLiteral("minimumSpacingPt")).toDouble(spec.minimumSpacingPt);
    spec.safeAreaInsetPt = object.value(QStringLiteral("safeAreaInsetPt")).toDouble(spec.safeAreaInsetPt);
    spec.includeCorners = object.value(QStringLiteral("includeCorners")).toBool(spec.includeCorners);
    return spec;
}

PDFGrommetPlacementReport placeGrommets(const QRectF& productionRect, const PDFGrommetSpec& spec)
{
    PDFGrommetPlacementReport report;
    if (!productionRect.isValid() || productionRect.isEmpty())
    {
        addDiagnostic(report.diagnostics, QStringLiteral("production.grommet.invalid_geometry"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Grommet placement requires a non-empty production rectangle."));
        return report;
    }
    if (!(spec.diameterPt > 0.0) || !(spec.targetSpacingPt > 0.0) || spec.minimumSpacingPt < 0.0 || spec.edgeOffsetPt < 0.0)
    {
        addDiagnostic(report.diagnostics, QStringLiteral("production.grommet.invalid_spec"), PDFProductionDiagnosticSeverity::Error,
                      QStringLiteral("Grommet diameter, spacing, and offsets must be valid positive values."));
        return report;
    }

    const QRectF rect = productionRect.normalized();
    const QLineF top(rect.topLeft(), rect.topRight());
    const QLineF right(rect.topRight(), rect.bottomRight());
    const QLineF bottom(rect.bottomRight(), rect.bottomLeft());
    const QLineF left(rect.bottomLeft(), rect.topLeft());
    appendEdgePoints(report.points, top, spec, true);
    appendEdgePoints(report.points, right, spec, true);
    appendEdgePoints(report.points, bottom, spec, true);
    appendEdgePoints(report.points, left, spec, true);

    const double minimumSpacing = qMax(spec.minimumSpacingPt, spec.diameterPt);
    for (int first = 0; first < report.points.size(); ++first)
    {
        for (int second = first + 1; second < report.points.size(); ++second)
        {
            if (QLineF(report.points.at(first), report.points.at(second)).length() + 0.000001 < minimumSpacing)
            {
                addDiagnostic(report.diagnostics, QStringLiteral("production.grommet.collision"), PDFProductionDiagnosticSeverity::Error,
                              QStringLiteral("Grommet spacing violates the minimum collision distance."));
            }
        }
    }
    return report;
}

QJsonObject PDFGrommetPlacementReport::toJson() const
{
    QJsonArray pointsJson;
    for (const QPointF& point : points)
    {
        pointsJson.append(QJsonObject{{ QStringLiteral("x"), point.x() }, { QStringLiteral("y"), point.y() }});
    }
    return QJsonObject{{ QStringLiteral("count"), points.size() },
                        { QStringLiteral("points"), pointsJson },
                        { QStringLiteral("diagnostics"), diagnosticsToJson(diagnostics) }};
}

} // namespace pdf
