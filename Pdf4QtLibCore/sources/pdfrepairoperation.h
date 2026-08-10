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

#ifndef PDFREPAIROPERATION_H
#define PDFREPAIROPERATION_H

#include "pdfrepairdiff.h"

#if defined(_MSC_VER)
#pragma push_macro("analyze")
#pragma push_macro("apply")
#undef analyze
#undef apply
#endif

#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <map>
#include <memory>

namespace pdf
{

enum class PDFRepairStatus
{
    Planned,
    Applied,
    Passed,
    Failed,
    Incomplete,
    Unsupported,
    Cancelled
};

enum class PDFRepairRisk
{
    Low,
    Medium,
    High,
    Destructive
};

enum class PDFRepairDomain : quint32
{
    None = 0,
    PageGeometry = 1u << 0,
    Text = 1u << 1,
    Fonts = 1u << 2,
    Vector = 1u << 3,
    Paths = 1u << 4,
    Images = 1u << 5,
    Color = 1u << 6,
    Layers = 1u << 7,
    Annotations = 1u << 8,
    Metadata = 1u << 9,
    Structure = 1u << 10
};
Q_DECLARE_FLAGS(PDFRepairDomains, PDFRepairDomain)
Q_DECLARE_OPERATORS_FOR_FLAGS(PDFRepairDomains)

enum class PDFRepairValidatorKind
{
    StructuralIntegrity,
    NormalPreflight,
    ImageResolution,
    ColorMode,
    OutputIntent,
    FontIntegrity,
    TextExtraction,
    SignatureState,
    Custom
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairTarget
{
    int pageIndex = -1;
    PDFObjectReference objectReference;
    QString semanticPath;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairPlan
{
    QString operationId;
    int operationVersion = 1;
    QJsonObject parameters;
    PDFRepairRisk risk = PDFRepairRisk::Medium;
    PDFRepairDomains domains;
    QList<PDFRepairTarget> targets;
    QStringList preconditions;
    QStringList warnings;
    QStringList unsupportedReasons;
    PDFRepairExpectedChanges expectedChanges;
    QList<PDFRepairValidatorKind> validators;
    bool requiresPreview = true;
    bool requiresPostflight = true;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairChange
{
    PDFRepairTarget target;
    QString changeKind;
    QString beforeSummary;
    QString afterSummary;
    bool expected = true;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairValidationResult
{
    PDFRepairStatus status = PDFRepairStatus::Incomplete;
    QString validatorId;
    QString summary;
    QStringList evidence;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairFindingDelta
{
    QStringList resolvedFindingIds;
    QStringList unchangedFindingIds;
    QStringList introducedFindingIds;
    QStringList incompleteFindingIds;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairResult
{
    PDFRepairStatus status = PDFRepairStatus::Failed;
    QString operationId;
    QList<PDFRepairChange> changes;
    QStringList warnings;
    QStringList incompleteReasons;
    QStringList validationFailures;
    QList<PDFRepairValidationResult> validations;
    PDFRepairFindingDelta findingDelta;

    QJsonObject toJson() const;
};

class PDF4QTLIBCORESHARED_EXPORT PDFRepairOperation
{
public:
    virtual ~PDFRepairOperation() = default;

    virtual QString id() const = 0;
    virtual int version() const { return 1; }
    virtual PDFRepairRisk risk() const = 0;
    virtual PDFRepairDomains domains() const = 0;
    /// JSON Schema fragment for the operation parameters.  Action Lists use
    /// this metadata to validate a complete recipe before any mutation.
    virtual QJsonObject parameterSchema() const;

    virtual PDFOperationResult analyze(const PDFDocument& source,
                                       const QJsonObject& parameters,
                                       PDFRepairPlan* plan) const = 0;

    virtual PDFOperationResult apply(PDFDocument* candidate,
                                     const PDFRepairPlan& plan,
                                     PDFRepairResult* result) const = 0;

    QJsonObject descriptor() const;
};

class PDF4QTLIBCORESHARED_EXPORT PDFRepairRegistry
{
public:
    static PDFRepairRegistry& instance();

    void registerOperation(std::unique_ptr<PDFRepairOperation> operation);
    const PDFRepairOperation* find(const QString& operationId) const;
    QStringList operationIds() const;
    QJsonArray descriptors() const;

private:
    PDFRepairRegistry() = default;
    PDFRepairRegistry(const PDFRepairRegistry&) = delete;
    PDFRepairRegistry& operator=(const PDFRepairRegistry&) = delete;
    std::map<QString, std::unique_ptr<PDFRepairOperation>> m_operations;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRepairTransactionOptions
{
    bool requirePreview = true;
    bool requirePostflight = true;
    bool failOnUnexpectedDiff = true;
    bool failOnIncompleteValidation = true;
    int maxOperations = 100;
    const PDFOperationControl* operationControl = nullptr;
};

class PDF4QTLIBCORESHARED_EXPORT PDFRepairTransaction
{
public:
    explicit PDFRepairTransaction(const PDFDocument& source,
                                  PDFRepairTransactionOptions options = {});

    PDFOperationResult add(const PDFRepairOperation* operation,
                           const QJsonObject& parameters);
    PDFOperationResult analyze();
    PDFOperationResult apply();

    PDFOperationResult serializeCandidate(const QString& candidatePath,
                                           PDFDocument* reopenedCandidate,
                                           QByteArray* candidateSha256 = nullptr) const;
    PDFOperationResult compareCandidate(const QString& candidatePath,
                                        PDFRepairDiffOptions options,
                                        PDFRepairDiffReport* report);

    const PDFDocument* candidate() const;
    const QList<PDFRepairPlan>& plans() const { return m_plans; }
    const QList<PDFRepairResult>& results() const { return m_results; }
    PDFRepairStatus status() const { return m_status; }

private:
    struct Entry
    {
        const PDFRepairOperation* operation = nullptr;
        QJsonObject parameters;
    };

    PDFRepairExpectedChanges expectedChanges() const;
    QVector<int> affectedPages() const;

    const PDFDocument* m_source = nullptr;
    PDFRepairTransactionOptions m_options;
    QList<Entry> m_entries;
    QList<PDFRepairPlan> m_plans;
    QList<PDFRepairResult> m_results;
    PDFDocument m_candidate;
    PDFRepairStatus m_status = PDFRepairStatus::Planned;
    bool m_analyzed = false;
    bool m_hasCandidate = false;
};

QString pdfRepairStatusName(PDFRepairStatus status);
QString pdfRepairRiskName(PDFRepairRisk risk);
QString pdfRepairDomainName(PDFRepairDomain domain);
QString pdfRepairValidatorName(PDFRepairValidatorKind validator);

} // namespace pdf

Q_DECLARE_OPERATORS_FOR_FLAGS(pdf::PDFRepairDomains)

#if defined(_MSC_VER)
#pragma pop_macro("apply")
#pragma pop_macro("analyze")
#endif

#endif // PDFREPAIROPERATION_H
