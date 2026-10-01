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

#ifndef PDFSTANDARDCONVERSION_H
#define PDFSTANDARDCONVERSION_H

#include "pdfdocument.h"
#include "pdfglobal.h"
#include "pdftransparencyflattener.h"
#include "pdfoperationcontrol.h"
#include "pdfutils.h"   // PDFOperationResult, returned by preview()/prepare() below

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

namespace pdf
{

enum class PDFStandardTarget
{
    PDFX1a2001,
    PDFX3_2002,
    PDFX4,
    PDFA2b
};

/// Whether a conversion flattens live transparency. `Automatic` follows the
/// target's own rule (PDF/X-1a:2001 and PDF/X-3:2002 forbid live transparency,
/// PDF/X-4 and PDF/A-2b permit it); the other two values are an operator's
/// explicit instruction and are honoured even when they contradict that rule.
enum class PDFTransparencyFlattenPolicy
{
    Automatic,
    Always,
    Never
};

LOOPLIBCORESHARED_EXPORT QString pdfStandardTargetToString(PDFStandardTarget target);
LOOPLIBCORESHARED_EXPORT bool pdfStandardTargetFromString(const QString& value,
                                                          PDFStandardTarget* target);
LOOPLIBCORESHARED_EXPORT QStringList supportedPDFStandardTargets();

struct LOOPLIBCORESHARED_EXPORT PDFStandardConversionSettings
{
    PDFStandardTarget target = PDFStandardTarget::PDFX4;
    QByteArray outputIntentIccData;
    QByteArray outputIntentIccId;
    QString outputIntentName;
    bool normalizeColor = false;
    bool blackPointCompensation = true;
    PDFTransparencyFlattenPolicy transparencyFlatten = PDFTransparencyFlattenPolicy::Automatic;
    PDFTransparencyFlattenSettings transparencyFlattenSettings;
    QString independentValidatorProgram;
    QStringList independentValidatorArguments;
    int independentValidatorTimeoutMs = 120000;
    bool dryRunOnly = false;
};

/// True when \p settings ask for live transparency to be flattened, following
/// the target default only when the policy is Automatic. Preview, the
/// preflight-blocker classification, the operation plan's expected changes, and
/// preparation path must all use this one answer.
LOOPLIBCORESHARED_EXPORT bool flattensTransparency(const PDFStandardConversionSettings& settings);

struct LOOPLIBCORESHARED_EXPORT PDFStandardConversionChange
{
    QString id;
    QString before;
    QString after;

    QJsonObject toJson() const;
};

struct LOOPLIBCORESHARED_EXPORT PDFStandardConversionReport
{
    QString target;
    bool conversionAttempted = false;
    bool independentValidationPassed = false;
    bool postflightPassed = false;
    QJsonObject preflightBefore;
    QJsonObject postflightAfter;
    QVector<PDFStandardConversionChange> changes;
    QStringList blockers;
    QStringList warnings;
    QJsonObject validator;
    QJsonObject transparencyFlatten;

    QJsonObject toJson() const;
};

enum class PDFArtifactValidationStatus
{
    Passed,
    Rejected,
    Incomplete
};

struct LOOPLIBCORESHARED_EXPORT PDFArtifactValidationResult
{
    PDFArtifactValidationStatus status = PDFArtifactValidationStatus::Incomplete;
    QString target;
    QString artifactSha256;
    qint64 artifactBytes = 0;
    QString validatorProgram;
    QString validatorVersion;
    QString reportSha256;
    QString reason;
    QJsonObject evidence;

    QJsonObject toJson() const;
};

LOOPLIBCORESHARED_EXPORT PDFStandardConversionSettings standardConversionSettings(const QJsonObject& parameters);

class LOOPLIBCORESHARED_EXPORT PDFStandardConversion
{
public:
    static PDFOperationResult preview(const PDFDocument* document,
                                      const PDFStandardConversionSettings& settings,
                                      PDFStandardConversionReport* report);

    static PDFOperationResult prepare(PDFDocument* document,
                                      const PDFStandardConversionSettings& settings,
                                      PDFStandardConversionReport* report = nullptr);

    static PDFArtifactValidationResult validateArtifact(const QByteArray& bytes,
                                                        const PDFStandardConversionSettings& settings,
                                                        const PDFOperationControl* control = nullptr);
    static PDFOperationResult validateArtifacts(const QByteArray& bytes,
                                                const QList<PDFStandardConversionSettings>& requirements,
                                                QJsonArray* evidence,
                                                const PDFOperationControl* control = nullptr);
    static PDFOperationResult writeCandidate(const PDFDocument& document,
                                             const QString& path,
                                             const QList<PDFStandardConversionSettings>& requirements,
                                             PDFDocument* reopened,
                                             QByteArray* bytes,
                                             QJsonArray* evidence,
                                             const PDFOperationControl* control = nullptr);
};

}   // namespace pdf

#endif   // PDFSTANDARDCONVERSION_H
