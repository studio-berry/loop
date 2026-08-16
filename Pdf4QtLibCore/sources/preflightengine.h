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

#ifndef PREFLIGHTENGINE_H
#define PREFLIGHTENGINE_H

#include "pdfglobal.h"
#include "pdfdocumentsession.h"
#include "pdfevidencegraph.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <map>
#include <optional>

namespace pdf
{

/// Report contract version emitted by PreflightResult::toJson().
inline constexpr int PREFLIGHT_REPORT_SCHEMA_VERSION = 3;

/// Finding location scope in normalized preflight reports.
inline constexpr QLatin1String PREFLIGHT_FINDING_SCOPE_DOCUMENT("document");
inline constexpr QLatin1String PREFLIGHT_FINDING_SCOPE_PAGE("page");
inline constexpr QLatin1String PREFLIGHT_FINDING_SCOPE_OBJECT("object");

/// PDF/X target supported by the declarative policy layer.
enum class PDFXFlavor
{
    None,
    X1a2001,
    X3_2002,
    X4
};

/// Deterministic reduction of mandatory PDF/X rule states.
enum class PDFXConformanceStatus
{
    Conformant,
    NonConformant,
    Incomplete
};

/// State of one PDF/X policy rule.
enum class PDFXRuleState
{
    Passed,
    Failed,
    NotInspected,
    NotApplicable
};

/// One auditable requirement in a PDF/X policy pack.
struct PDF4QTLIBCORESHARED_EXPORT PDFXRuleRequirement
{
    QString ruleId;
    bool mandatory = true;
};

/// Immutable metadata and requirements for one PDF/X target.
struct PDF4QTLIBCORESHARED_EXPORT PDFXPolicy
{
    PDFXFlavor flavor = PDFXFlavor::None;
    QString policyVersion;
    QVector<PDFXRuleRequirement> rules;
};

/// Structured result from one PDF/X policy rule.
struct PDF4QTLIBCORESHARED_EXPORT PDFXRuleResult
{
    QString ruleId;
    bool mandatory = true;
    PDFXRuleState state = PDFXRuleState::NotInspected;
    QJsonObject evidence;
    QString diagnostic;
};

/// Normalized, deterministic PDF/X conformance result.
struct PDF4QTLIBCORESHARED_EXPORT PDFXConformanceResult
{
    PDFXFlavor requestedFlavor = PDFXFlavor::None;
    PDFXConformanceStatus status = PDFXConformanceStatus::Incomplete;
    QString policyVersion;
    QStringList failedRuleIds;
    QStringList incompleteRuleIds;
    QVector<PDFXRuleResult> rules;

    QJsonObject toJson() const;
};

/// Returns the stable display/profile name for a PDF/X flavor.
PDF4QTLIBCORESHARED_EXPORT QString pdfxFlavorToString(PDFXFlavor flavor);

/// Returns the explicitly supported PDF/X profile targets.
PDF4QTLIBCORESHARED_EXPORT QStringList supportedPDFXTargets();

/// Resolves a target name to the audited policy registry entry.
PDF4QTLIBCORESHARED_EXPORT bool pdfxPolicyForTarget(const QString& target,
                                                    PDFXPolicy& policy,
                                                    QString& errorMessage);

/// Reduces mandatory PDF/X rule states. A definite failure takes precedence
/// over missing evidence; a mandatory not-applicable rule is incomplete.
PDF4QTLIBCORESHARED_EXPORT PDFXConformanceStatus reducePDFXStatus(const QVector<PDFXRuleResult>& rules,
                                                                  QStringList* failedRuleIds = nullptr,
                                                                  QStringList* incompleteRuleIds = nullptr);

/// Configuration for a single preflight check, parsed from a profile.
struct PDF4QTLIBCORESHARED_EXPORT PreflightCheckConfig
{
    QString id;
    QString severity = QStringLiteral("error");
    bool enabled = true;
    qreal amountPt = 9.0;
    bool required = true;
    qreal expectedWidthPt = 0.0;
    qreal expectedHeightPt = 0.0;
    qreal tolerancePt = 1.0;
    bool hasExpectedSize = false;

    // Raster probe parameters shared by content-bleed and ink-coverage.
    bool rasterConfirm = false;
    int probeDpi = 150;
    int probeThreshold = 16;
    qreal rasterWhiteThreshold = 0.9975;

    // ink-coverage parameters.
    qreal maxInkPct = 0.0;
    qreal minRegionAreaPct = 0.05;
    int maxRegionsPerPage = 20;
    qint64 maxRasterPixels = 250LL * 1000 * 1000;
    QString inkCoverageAnalysisBox = QStringLiteral("bleed");

    // image-resolution parameters.
    int minDpi = 0;

    // color-mode parameters (e.g. ["CMYK", "Grayscale"]).
    QStringList allowedColorModes;

    // Production processing-step requirements. Values use the normalized
    // PDFProcessingStepType names, for example "cutting-die".
    QStringList requiredProcessingStepTypes;

    // color-inventory parameters.
    int colorProbeDpi = 150;
    qreal richBlackKThreshold = 0.10;

    // output-intent parameters (optional allow-list of /OutputConditionIdentifier values).
    QStringList allowedOutputConditionIdentifiers;
    QStringList allowedOutputIntentSubtypes;
    QStringList allowedOutputIntentProfileSha256;
    bool requireEmbeddedOutputIntentProfile = true;
    bool allowMultipleOutputIntents = true;

    // thin-strokes parameters.
    qreal minEffectiveStrokeWidthPt = 0.0;
    qreal zeroWidthEpsilonPt = 1.0e-6;
    QString hairlineSeverity;
    QString thinStrokeSeverity;

    // thin-parts parameters. The default classes preserve the stroke/fill
    // inspection surface; clipped parts and negative space are opt-in.
    QStringList thinPartClasses;
    QMap<QString, QString> thinPartSeverityByClass;
};

/// Configuration for a single advertised fixup, parsed from a profile.
struct PDF4QTLIBCORESHARED_EXPORT PreflightFixupConfig
{
    QString id;
    bool confirm = true;
    qreal amountPt = 0.0;
    QString description;
    QJsonObject params;
};

/// A single preflight finding (error or warning).
struct PDF4QTLIBCORESHARED_EXPORT PreflightFinding
{
    QString scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
    int page = 1;
    QString objectId;
    QString type;
    QString severity;
    QString message;
    QRectF bbox;
    QString checkId;
    QJsonObject evidence;
    QStringList evidenceIds;

    /// Stable identity for this finding. The identity excludes translated
    /// message text and geometry so it survives locale changes and fixups.
    QString stableId() const;
};

/// Operator decision recorded against one finding and one inspected state.
enum class PreflightDecisionKind
{
    Accept,
    Waive,
    Override,
    Reject,
    Reopen
};

/// Derived state of a decision against the current document/profile pair.
enum class PreflightDecisionState
{
    Active,
    StaleDocument,
    StaleProfile,
    Invalid
};

inline constexpr int PREFLIGHT_DECISION_MIN_JUSTIFICATION_LENGTH = 3;

struct PDF4QTLIBCORESHARED_EXPORT PreflightDecision
{
    QString findingId;
    PreflightDecisionKind kind = PreflightDecisionKind::Accept;
    QString justification;
    QString operatorIdentity;
    QDateTime timestampUtc;
    QString externalReference;
    QString documentRevisionDigest;
    QString effectiveProfileDigest;

    /// Serializes the decision and derives state from the current digests.
    QJsonObject toJson(const QString& currentDocumentDigest = QString(),
                       const QString& currentProfileDigest = QString()) const;

    /// Parses and validates one imported decision. Stored state is ignored;
    /// state is always derived against the current run.
    static bool fromJson(const QJsonObject& object,
                         PreflightDecision& decision,
                         QString& errorMessage);

    PreflightDecisionState resolveState(const QString& currentDocumentDigest,
                                        const QString& currentProfileDigest) const;

    bool countsForSignoff(const QString& currentDocumentDigest,
                          const QString& currentProfileDigest) const;
};

PDF4QTLIBCORESHARED_EXPORT QString preflightDecisionKindToString(PreflightDecisionKind kind);
PDF4QTLIBCORESHARED_EXPORT bool preflightDecisionKindFromString(const QString& value,
                                                                PreflightDecisionKind& kind);
PDF4QTLIBCORESHARED_EXPORT QString preflightDecisionStateToString(PreflightDecisionState state);

/// Standalone decision-file contract used by PdfTool import/export.
PDF4QTLIBCORESHARED_EXPORT QJsonObject preflightDecisionsToJson(const QList<PreflightDecision>& decisions);
PDF4QTLIBCORESHARED_EXPORT bool preflightDecisionsFromJson(const QJsonObject& object,
                                                           QList<PreflightDecision>& decisions,
                                                           QString& errorMessage);

/// Parsed preflight profile.
struct PDF4QTLIBCORESHARED_EXPORT PreflightProfileData
{
    QString name;
    QString id;
    QString version;
    QString authored;
    QString derivedFrom;
    QJsonObject restrictions;
    QJsonObject variables;
    QList<PreflightCheckConfig> checks;
    QList<PreflightFixupConfig> fixups;
    std::optional<PDFXPolicy> pdfx;
};

/// Per-check execution status in schema v3 reports.
struct PDF4QTLIBCORESHARED_EXPORT PreflightCheckStatus
{
    QString id;
    QString status;
    QString reason;
    QString budgetKind;
    qint64 budgetLimit = 0;
    qint64 budgetAttempted = 0;
    QString budgetContext;
};

/// Result of a preflight run.
struct PDF4QTLIBCORESHARED_EXPORT PreflightResult
{
    /// Legacy convenience value. Callers must use reducePreflightVerdict().
    bool pass = true;
    bool inspectionComplete = true;
    QString errorCode;
    QString errorMessage;
    QString profileName;
    QList<PreflightFinding> errors;
    QList<PreflightFinding> warnings;
    QList<PreflightFixupConfig> fixupsAvailable;
    QList<PreflightCheckStatus> checkStatuses;
    std::optional<PDFXConformanceResult> pdfx;
    QJsonObject profileResolution;
    QString documentRevisionDigest;
    QString effectiveProfileDigest;
    QList<PreflightDecision> decisions;

    QJsonObject toJson(const QString& pdfPath = QString()) const;
};

/// Orchestrator that runs a declarative preflight profile against a document
/// session and produces a normalized JSON report.
///
/// Checks register by string id. The engine ships with built-in Tier-1 checks
/// (bleed, trim, page-size). Additional checks (for example, content-bleed) can
/// be registered before calling run().
class PDF4QTLIBCORESHARED_EXPORT PreflightEngine
{
public:
    using CheckRunner = std::function<void(PDFDocumentSession* session,
                                           const PreflightCheckConfig& check,
                                           QList<PreflightFinding>& errors,
                                           QList<PreflightFinding>& warnings)>;

    explicit PreflightEngine(PDFDocumentSession* session);

    /// Registers a check implementation. Overwrites any previous registration
    /// for the same id.
    void registerCheck(const QString& id, CheckRunner runner);

    /// Returns true if a check has been registered for the given id.
    bool hasCheck(const QString& id) const;

    /// Runs the profile against the document session and returns findings plus
    /// the advertised fixups. Errors (severity "error") cause pass == false.
    PreflightResult run(const QJsonObject& profile);
    PreflightResult run(const PreflightProfileData& profile);

    PDFDocumentSession* getSession() const;
    const PDFEvidenceGraph& lastEvidenceGraph() const;

    /// Loads a profile JSON file. On error, returns false and writes a message
    /// to \p errorMessage.
    static bool loadProfile(const QString& profilePath, QJsonObject& profile, QString& errorMessage);

    /// Parses a profile JSON object into the engine's internal struct. On error,
    /// returns false and writes a message to \p errorMessage.
    static bool parseProfile(const QJsonObject& profileObject, PreflightProfileData& profile, QString& errorMessage);

private:
    void registerBuiltInChecks();

    PDFDocumentSession* m_session;
    std::map<QString, CheckRunner> m_checks;
    PDFEvidenceGraph m_activeGraph;
};

}   // namespace pdf

#endif   // PREFLIGHTENGINE_H
