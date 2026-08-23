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

#ifndef PDFPRODUCTIONGEOMETRY_H
#define PDFPRODUCTIONGEOMETRY_H

#include "pdfglobal.h"

#include <QJsonObject>
#include <QList>
#include <QVariantMap>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QVector>

namespace pdf
{

class PDFDocument;

constexpr int PDFProductionGeometrySchemaVersion = 1;

enum class PDFProductionDiagnosticSeverity
{
    Info,
    Warning,
    Error
};

enum class PDFProcessingStepKind
{
    Cut,
    Crease,
    Perforation,
    WhiteInk,
    Varnish,
    Registration,
    TechnicalNonPrinting,
    Custom
};

LOUPELIBCORESHARED_EXPORT QString pdfProcessingStepKindToString(PDFProcessingStepKind kind);
LOUPELIBCORESHARED_EXPORT PDFProcessingStepKind pdfProcessingStepKindFromString(const QString& value);

/// ISO 19593-1 processing-step classification. The production kind remains
/// the stable application-facing compatibility category.
enum class PDFProcessingStepType
{
    CuttingDie,
    PerforatingCut,
    CreasingBend,
    PartialCut,
    ScoringBend,
    ForegroundVarnish,
    Braille,
    White,
    Legend,
    Positions,
    PositionsUnspecified,
    Unknown
};

LOUPELIBCORESHARED_EXPORT QString pdfProcessingStepTypeToString(PDFProcessingStepType type);
LOUPELIBCORESHARED_EXPORT PDFProcessingStepType pdfProcessingStepTypeFromString(const QString& value);

struct LOUPELIBCORESHARED_EXPORT PDFProductionDiagnostic
{
    QString id;
    PDFProductionDiagnosticSeverity severity = PDFProductionDiagnosticSeverity::Warning;
    QString message;
    int pageIndex = -1;
    QString objectId;

    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFProductionContour
{
    QString id;
    int pageIndex = -1;
    QPainterPath path;
    bool closed = true;
    bool hole = false;
    double flatteningTolerancePt = 0.1;
    QString sourceObjectRef;
    QString sourceEvidence;

    QJsonObject toJson() const;
    static PDFProductionContour fromJson(const QJsonObject& object);
};

struct LOUPELIBCORESHARED_EXPORT PDFProcessingStep
{
    QString id;
    PDFProcessingStepKind kind = PDFProcessingStepKind::Custom;
    QString displayName;
    QString spotColorName;
    bool shouldPrint = true;
    bool overprint = false;
    QVariantMap vendorMetadata;

    // Optional source-document evidence populated by detectProcessingSteps().
    PDFProcessingStepType type = PDFProcessingStepType::Unknown;
    QString ocgName;
    PDFObjectReference ocg;
    QPainterPath geometry;
    bool isSeparation = false;
    QString detectionMethod;
    QVector<int> pageIndices;

    QJsonObject toJson() const;
    static PDFProcessingStep fromJson(const QJsonObject& object);
};

struct LOUPELIBCORESHARED_EXPORT PDFProductionPath
{
    QString id;
    QPainterPath geometry;
    QString processingStepId;
    QString sourceEvidence;

    QJsonObject toJson() const;
    static PDFProductionPath fromJson(const QJsonObject& object);
};

struct LOUPELIBCORESHARED_EXPORT PDFProductionGeometryModel
{
    int schemaVersion = PDFProductionGeometrySchemaVersion;
    QVector<PDFProductionContour> contours;
    QVector<PDFProcessingStep> processingSteps;
    QVector<PDFProductionPath> processingPaths;
    QVariantMap vendorMetadata;

    QJsonObject toJson() const;
    static PDFProductionGeometryModel fromJson(const QJsonObject& object);
};

/// Names from PDF production workflows are adapters only.  They never infer a
/// processing step for a document; callers must explicitly assign the returned
/// kind to a typed production object.
class LOUPELIBCORESHARED_EXPORT PDFProcessingStepRegistry
{
public:
    static QString canonicalKindForAlias(const QString& alias);
    static bool isKnownAlias(const QString& alias);
};

struct LOUPELIBCORESHARED_EXPORT PDFProductionValidationOptions
{
    double tolerancePt = 0.1;
    int maxSegments = 100000;
    int maxContours = 1000;
};

struct LOUPELIBCORESHARED_EXPORT PDFProductionValidationReport
{
    bool valid = true;
    QVector<PDFProductionDiagnostic> diagnostics;
    int segmentCount = 0;

    QJsonObject toJson() const;
};

LOUPELIBCORESHARED_EXPORT PDFProductionValidationReport validateProductionGeometry(
        const PDFProductionGeometryModel& model,
        const PDFProductionValidationOptions& options = {});

/// Detects ISO 19593-1 processing-step OCGs and legacy dieline spot colors.
/// The returned geometry is flattened into page space and the detection
/// mechanism is explicit: "iso-19593-1" or "legacy-spot-color".
LOUPELIBCORESHARED_EXPORT QList<PDFProcessingStep> detectProcessingSteps(const PDFDocument& document);

struct LOUPELIBCORESHARED_EXPORT PDFContourBleedSettings
{
    double amountPt = 9.0;
    double flatteningTolerancePt = 0.1;
    int maxSegments = 100000;
};

struct LOUPELIBCORESHARED_EXPORT PDFContourBleedPlan
{
    bool valid = false;
    QString contourId;
    QRectF sourceBounds;
    QRectF bleedBounds;
    QPainterPath annularRing;
    int segmentCount = 0;
    QVector<PDFProductionDiagnostic> diagnostics;

    QJsonObject toJson() const;
};

LOUPELIBCORESHARED_EXPORT PDFContourBleedPlan planContourBleed(
        const PDFProductionContour& contour,
        const PDFContourBleedSettings& settings = {});

struct LOUPELIBCORESHARED_EXPORT PDFGrommetSpec
{
    double diameterPt = 25.5;
    double edgeOffsetPt = 36.0;
    double targetSpacingPt = 864.0;
    double minimumSpacingPt = 72.0;
    double safeAreaInsetPt = 0.0;
    bool includeCorners = true;

    QJsonObject toJson() const;
    static PDFGrommetSpec fromJson(const QJsonObject& object);
};

struct LOUPELIBCORESHARED_EXPORT PDFGrommetPlacementReport
{
    QVector<QPointF> points;
    QVector<PDFProductionDiagnostic> diagnostics;

    QJsonObject toJson() const;
};

LOUPELIBCORESHARED_EXPORT PDFGrommetPlacementReport placeGrommets(
        const QRectF& productionRect,
        const PDFGrommetSpec& spec = {});

} // namespace pdf

#endif // PDFPRODUCTIONGEOMETRY_H
