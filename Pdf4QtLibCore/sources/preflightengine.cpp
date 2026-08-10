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

#include "preflightengine.h"
#include "pdfpreflightverdict.h"

#include "pdfbleedmarginprobe.h"
#include "pdfblendfunction.h"
#include "pdfcatalog.h"
#include "pdfcms.h"
#include "pdfcolorinventory.h"
#include "pdfcolorspaces.h"
#include "pdfconstants.h"
#include "pdfdocument.h"
#include "pdfexception.h"
#include "pdffont.h"
#include "pdffontintegrity.h"
#include "pdffixupregistry.h"
#include "pdfglobal.h"
#include "pdfimage.h"
#include "pdfimageoptimizer.h"
#include "pdfinkcoverageprobe.h"
#include "pdfmeshqualitysettings.h"
#include "pdfoptionalcontent.h"
#include "pdfpage.h"
#include "pdfpagecontentprocessor.h"
#include "pdfpattern.h"
#include "pdfpreflightchecks.h"
#include "pdfprocessingbudget.h"
#include "pdffixupregistry.h"
#include "pdfproductiongeometry.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QSet>

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <set>

namespace pdf
{

namespace
{

bool isSha256Digest(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return expression.match(value).hasMatch();
}

} // namespace

QString PreflightFinding::stableId() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(checkId.toUtf8());
    hash.addData("\x1f", 1);
    hash.addData(scope.toUtf8());
    hash.addData("\x1f", 1);
    hash.addData(QByteArray::number(page));
    hash.addData("\x1f", 1);
    hash.addData(objectId.toUtf8());
    hash.addData("\x1f", 1);
    hash.addData(type.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(16));
}

QString preflightDecisionKindToString(PreflightDecisionKind kind)
{
    switch (kind)
    {
        case PreflightDecisionKind::Accept:
            return QStringLiteral("accept");
        case PreflightDecisionKind::Waive:
            return QStringLiteral("waive");
        case PreflightDecisionKind::Override:
            return QStringLiteral("override");
        case PreflightDecisionKind::Reject:
            return QStringLiteral("reject");
        case PreflightDecisionKind::Reopen:
            return QStringLiteral("reopen");
    }

    return QString();
}

bool preflightDecisionKindFromString(const QString& value, PreflightDecisionKind& kind)
{
    const QString normalized = value.trimmed().toLower();
    const QList<QPair<QString, PreflightDecisionKind>> values = {
        { QStringLiteral("accept"), PreflightDecisionKind::Accept },
        { QStringLiteral("waive"), PreflightDecisionKind::Waive },
        { QStringLiteral("override"), PreflightDecisionKind::Override },
        { QStringLiteral("reject"), PreflightDecisionKind::Reject },
        { QStringLiteral("reopen"), PreflightDecisionKind::Reopen }
    };
    for (const auto& valuePair : values)
    {
        if (normalized == valuePair.first)
        {
            kind = valuePair.second;
            return true;
        }
    }

    return false;
}

QString preflightDecisionStateToString(PreflightDecisionState state)
{
    switch (state)
    {
        case PreflightDecisionState::Active:
            return QStringLiteral("active");
        case PreflightDecisionState::StaleDocument:
            return QStringLiteral("stale_document");
        case PreflightDecisionState::StaleProfile:
            return QStringLiteral("stale_profile");
        case PreflightDecisionState::Invalid:
            return QStringLiteral("invalid");
    }

    return QStringLiteral("invalid");
}

PreflightDecisionState PreflightDecision::resolveState(const QString& currentDocumentDigest,
                                                       const QString& currentProfileDigest) const
{
    if (findingId.trimmed().isEmpty()
        || justification.trimmed().size() < PREFLIGHT_DECISION_MIN_JUSTIFICATION_LENGTH
        || operatorIdentity.trimmed().isEmpty()
        || !timestampUtc.isValid()
        || !isSha256Digest(documentRevisionDigest)
        || !isSha256Digest(effectiveProfileDigest)
        || !isSha256Digest(currentDocumentDigest)
        || !isSha256Digest(currentProfileDigest))
    {
        return PreflightDecisionState::Invalid;
    }

    if (documentRevisionDigest.compare(currentDocumentDigest, Qt::CaseInsensitive) != 0)
    {
        return PreflightDecisionState::StaleDocument;
    }
    if (effectiveProfileDigest.compare(currentProfileDigest, Qt::CaseInsensitive) != 0)
    {
        return PreflightDecisionState::StaleProfile;
    }

    return PreflightDecisionState::Active;
}

bool PreflightDecision::countsForSignoff(const QString& currentDocumentDigest,
                                         const QString& currentProfileDigest) const
{
    const PreflightDecisionState state = resolveState(currentDocumentDigest, currentProfileDigest);
    return state == PreflightDecisionState::Active
        && (kind == PreflightDecisionKind::Accept
            || kind == PreflightDecisionKind::Waive
            || kind == PreflightDecisionKind::Override);
}

QJsonObject PreflightDecision::toJson(const QString& currentDocumentDigest,
                                     const QString& currentProfileDigest) const
{
    QJsonObject object{
        { QStringLiteral("finding_id"), findingId },
        { QStringLiteral("kind"), preflightDecisionKindToString(kind) },
        { QStringLiteral("justification"), justification },
        { QStringLiteral("operator"), operatorIdentity },
        { QStringLiteral("timestamp_utc"), timestampUtc.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("document_revision_digest"), documentRevisionDigest },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest }
    };

    if (!externalReference.trimmed().isEmpty())
    {
        object.insert(QStringLiteral("external_reference"), externalReference);
    }
    if (!currentDocumentDigest.isEmpty() || !currentProfileDigest.isEmpty())
    {
        object.insert(QStringLiteral("state"), preflightDecisionStateToString(resolveState(currentDocumentDigest, currentProfileDigest)));
    }

    return object;
}

bool PreflightDecision::fromJson(const QJsonObject& object,
                                 PreflightDecision& decision,
                                 QString& errorMessage)
{
    decision = PreflightDecision();
    errorMessage.clear();

    decision.findingId = object.value(QStringLiteral("finding_id")).toString().trimmed();
    if (decision.findingId.isEmpty())
    {
        errorMessage = QStringLiteral("Decision is missing finding_id.");
        return false;
    }

    if (!preflightDecisionKindFromString(object.value(QStringLiteral("kind")).toString(), decision.kind))
    {
        errorMessage = QStringLiteral("Decision kind is invalid.");
        return false;
    }

    decision.justification = object.value(QStringLiteral("justification")).toString().trimmed();
    if (decision.justification.size() < PREFLIGHT_DECISION_MIN_JUSTIFICATION_LENGTH)
    {
        errorMessage = QStringLiteral("Decision justification must contain at least %1 non-whitespace characters.")
            .arg(PREFLIGHT_DECISION_MIN_JUSTIFICATION_LENGTH);
        return false;
    }

    decision.operatorIdentity = object.value(QStringLiteral("operator")).toString().trimmed();
    if (decision.operatorIdentity.isEmpty())
    {
        errorMessage = QStringLiteral("Decision is missing operator identity.");
        return false;
    }

    const QString timestamp = object.value(QStringLiteral("timestamp_utc")).toString();
    decision.timestampUtc = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!decision.timestampUtc.isValid())
    {
        decision.timestampUtc = QDateTime::fromString(timestamp, Qt::ISODate);
    }
    if (!decision.timestampUtc.isValid() || decision.timestampUtc.timeSpec() == Qt::LocalTime)
    {
        errorMessage = QStringLiteral("Decision timestamp_utc must be a valid ISO-8601 timestamp with an explicit UTC offset.");
        return false;
    }
    decision.timestampUtc = decision.timestampUtc.toUTC();

    decision.externalReference = object.value(QStringLiteral("external_reference")).toString().trimmed();
    decision.documentRevisionDigest = object.value(QStringLiteral("document_revision_digest")).toString().trimmed().toLower();
    decision.effectiveProfileDigest = object.value(QStringLiteral("effective_profile_digest")).toString().trimmed().toLower();
    if (!isSha256Digest(decision.documentRevisionDigest) || !isSha256Digest(decision.effectiveProfileDigest))
    {
        errorMessage = QStringLiteral("Decision document and profile digests must be 64-character SHA-256 values.");
        return false;
    }

    return true;
}

QJsonObject preflightDecisionsToJson(const QList<PreflightDecision>& decisions)
{
    QJsonArray array;
    for (const PreflightDecision& decision : decisions)
    {
        array.append(decision.toJson());
    }
    return QJsonObject{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("decisions"), array }
    };
}

bool preflightDecisionsFromJson(const QJsonObject& object,
                                QList<PreflightDecision>& decisions,
                                QString& errorMessage)
{
    decisions.clear();
    errorMessage.clear();
    if (object.value(QStringLiteral("schema_version")).toInt() != 1)
    {
        errorMessage = QStringLiteral("Decision file schema_version must be 1.");
        return false;
    }

    const QJsonValue value = object.value(QStringLiteral("decisions"));
    if (!value.isArray())
    {
        errorMessage = QStringLiteral("Decision file decisions must be an array.");
        return false;
    }

    const QJsonArray array = value.toArray();
    for (int index = 0; index < array.size(); ++index)
    {
        if (!array.at(index).isObject())
        {
            errorMessage = QStringLiteral("Decision %1 must be an object.").arg(index);
            decisions.clear();
            return false;
        }

        PreflightDecision decision;
        if (!PreflightDecision::fromJson(array.at(index).toObject(), decision, errorMessage))
        {
            errorMessage = QStringLiteral("Decision %1: %2").arg(index).arg(errorMessage);
            decisions.clear();
            return false;
        }
        decisions.append(decision);
    }

    return true;
}

QString pdfxFlavorToString(PDFXFlavor flavor)
{
    switch (flavor)
    {
        case PDFXFlavor::X1a2001:
            return QStringLiteral("PDF/X-1a:2001");
        case PDFXFlavor::X4:
            return QStringLiteral("PDF/X-4");
        case PDFXFlavor::X3_2002:
            return QStringLiteral("PDF/X-3:2002");
        case PDFXFlavor::None:
        default:
            return QString();
    }
}

QStringList supportedPDFXTargets()
{
    return {
        pdfxFlavorToString(PDFXFlavor::X1a2001),
        pdfxFlavorToString(PDFXFlavor::X4)
    };
}

bool pdfxPolicyForTarget(const QString& target, PDFXPolicy& policy, QString& errorMessage)
{
    errorMessage.clear();

    PDFXFlavor flavor = PDFXFlavor::None;
    if (target == pdfxFlavorToString(PDFXFlavor::X1a2001))
    {
        flavor = PDFXFlavor::X1a2001;
    }
    else if (target == pdfxFlavorToString(PDFXFlavor::X4))
    {
        flavor = PDFXFlavor::X4;
    }
    else
    {
        errorMessage = PDFTranslationContext::tr(
            "Unsupported PDF/X target '%1' (supported: %2).")
            .arg(target, supportedPDFXTargets().join(QStringLiteral(", ")));
        return false;
    }

    policy.flavor = flavor;
    policy.policyVersion = QStringLiteral("1");
    policy.rules = {
        { QStringLiteral("pdfx.document.version"), true },
        { QStringLiteral("pdfx.document.trailer-id"), true },
        { QStringLiteral("pdfx.document.encryption"), true },
        { QStringLiteral("pdfx.metadata.identification"), true },
        { QStringLiteral("pdfx.output-intent.present"), true },
        { QStringLiteral("pdfx.output-intent.identity"), true },
        { QStringLiteral("pdfx.output-intent.subtype"), true },
        { QStringLiteral("pdfx.output-intent.profile"), true },
        { QStringLiteral("pdfx.output-intent.profile-space"), true },
        { QStringLiteral("pdfx.page.trim-box"), true },
        { QStringLiteral("pdfx.page.bleed-box"), true },
        { QStringLiteral("pdfx.font.embedded"), true },
        { QStringLiteral("pdfx.color.device-rgb"), flavor == PDFXFlavor::X1a2001 },
        { QStringLiteral("pdfx.transparency.allowed"), true },
        { QStringLiteral("pdfx.overprint.inspectable"), true },
        { QStringLiteral("pdfx.annotation.forbidden-action"), true }
    };
    return true;
}

PDFXConformanceStatus reducePDFXStatus(const QVector<PDFXRuleResult>& rules,
                                       QStringList* failedRuleIds,
                                       QStringList* incompleteRuleIds)
{
    if (failedRuleIds)
    {
        failedRuleIds->clear();
    }
    if (incompleteRuleIds)
    {
        incompleteRuleIds->clear();
    }

    for (const PDFXRuleResult& rule : rules)
    {
        if (!rule.mandatory)
        {
            continue;
        }

        if (rule.state == PDFXRuleState::Failed)
        {
            if (failedRuleIds)
            {
                failedRuleIds->append(rule.ruleId);
            }
        }
        else if (rule.state == PDFXRuleState::NotInspected
                 || rule.state == PDFXRuleState::NotApplicable)
        {
            if (incompleteRuleIds)
            {
                incompleteRuleIds->append(rule.ruleId);
            }
        }
    }

    if (failedRuleIds && !failedRuleIds->isEmpty())
    {
        return PDFXConformanceStatus::NonConformant;
    }
    if (incompleteRuleIds && !incompleteRuleIds->isEmpty())
    {
        return PDFXConformanceStatus::Incomplete;
    }

    // Callers that do not request the ID lists still need the same reduction.
    const bool hasFailure = std::any_of(rules.cbegin(), rules.cend(), [](const PDFXRuleResult& rule) {
        return rule.mandatory && rule.state == PDFXRuleState::Failed;
    });
    if (hasFailure)
    {
        return PDFXConformanceStatus::NonConformant;
    }

    const bool incomplete = std::any_of(rules.cbegin(), rules.cend(), [](const PDFXRuleResult& rule) {
        return rule.mandatory
            && (rule.state == PDFXRuleState::NotInspected || rule.state == PDFXRuleState::NotApplicable);
    });
    return incomplete ? PDFXConformanceStatus::Incomplete : PDFXConformanceStatus::Conformant;
}

namespace
{

QString pdfxRuleStateToString(PDFXRuleState state)
{
    switch (state)
    {
        case PDFXRuleState::Passed:
            return QStringLiteral("passed");
        case PDFXRuleState::Failed:
            return QStringLiteral("failed");
        case PDFXRuleState::NotApplicable:
            return QStringLiteral("not-applicable");
        case PDFXRuleState::NotInspected:
        default:
            return QStringLiteral("not-inspected");
    }
}

} // namespace

QJsonObject PDFXConformanceResult::toJson() const
{
    QJsonArray failed;
    for (const QString& ruleId : failedRuleIds)
    {
        failed.append(ruleId);
    }

    QJsonArray incomplete;
    for (const QString& ruleId : incompleteRuleIds)
    {
        incomplete.append(ruleId);
    }

    QJsonArray ruleArray;
    for (const PDFXRuleResult& rule : rules)
    {
        QJsonObject ruleObject{
            { QStringLiteral("id"), rule.ruleId },
            { QStringLiteral("mandatory"), rule.mandatory },
            { QStringLiteral("state"), pdfxRuleStateToString(rule.state) }
        };
        if (!rule.evidence.isEmpty())
        {
            ruleObject.insert(QStringLiteral("evidence"), rule.evidence);
        }
        if (!rule.diagnostic.isEmpty())
        {
            ruleObject.insert(QStringLiteral("diagnostic"), rule.diagnostic);
        }
        ruleArray.append(ruleObject);
    }

    QString status;
    switch (this->status)
    {
        case PDFXConformanceStatus::Conformant:
            status = QStringLiteral("conformant");
            break;
        case PDFXConformanceStatus::NonConformant:
            status = QStringLiteral("non-conformant");
            break;
        case PDFXConformanceStatus::Incomplete:
        default:
            status = QStringLiteral("incomplete");
            break;
    }

    return QJsonObject{
        { QStringLiteral("target"), pdfxFlavorToString(requestedFlavor) },
        { QStringLiteral("policyVersion"), policyVersion },
        { QStringLiteral("status"), status },
        { QStringLiteral("failedRuleIds"), failed },
        { QStringLiteral("incompleteRuleIds"), incomplete },
        { QStringLiteral("rules"), ruleArray }
    };
}

namespace
{

QJsonArray rectToBbox(const QRectF& rect)
{
    return QJsonArray{ rect.left(), rect.top(), rect.right(), rect.bottom() };
}

bool hasMeaningfulBbox(const QRectF& rect)
{
    return rect.isValid() && rect.width() > 0.0 && rect.height() > 0.0;
}

QJsonObject findingToJson(const PreflightFinding& finding)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), finding.stableId());
    object.insert(QStringLiteral("scope"), finding.scope);
    object.insert(QStringLiteral("type"), finding.type);
    object.insert(QStringLiteral("severity"), finding.severity);
    object.insert(QStringLiteral("message"), finding.message);

    if (finding.scope != QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT))
    {
        object.insert(QStringLiteral("page"), finding.page);
    }

    if (!finding.objectId.isNull())
    {
        object.insert(QStringLiteral("object_id"), finding.objectId.isEmpty() ? QJsonValue::Null : QJsonValue(finding.objectId));
    }

    if (hasMeaningfulBbox(finding.bbox))
    {
        object.insert(QStringLiteral("bbox"), rectToBbox(finding.bbox));
    }

    if (!finding.checkId.isEmpty())
    {
        object.insert(QStringLiteral("check_id"), finding.checkId);
    }

    if (!finding.evidence.isEmpty())
    {
        object.insert(QStringLiteral("evidence"), finding.evidence);
    }

    return object;
}

constexpr int PREFLIGHT_MAX_FORM_DEPTH = 32;

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
        // Ignore invalid color space entries.
    }
}

void collectColorSpacesFromResources(const PDFDocument* document,
                                     const PDFObject& resourcesObject,
                                     QSet<QString>* paintedSpaces,
                                     std::set<PDFObjectReference>& visitedForms,
                                     int depth)
{
    if (!document || !paintedSpaces || depth > PREFLIGHT_MAX_FORM_DEPTH)
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
            // Only an indirect form can take part in a reference cycle, so that is
            // the only case worth de-duplicating. Do not call getReference() on
            // 'xobject': it is already dereferenced, and getReference() on a
            // non-reference throws std::bad_variant_access. Direct streams are
            // still bounded by PREFLIGHT_MAX_FORM_DEPTH.
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

QRectF imageBoundsFromCtm(const QTransform& ctm, int pixelWidth, int pixelHeight)
{
    const QPointF corners[4] = {
        ctm.map(QPointF(0, 0)),
        ctm.map(QPointF(pixelWidth, 0)),
        ctm.map(QPointF(pixelWidth, pixelHeight)),
        ctm.map(QPointF(0, pixelHeight))
    };

    QRectF bounds;
    for (const QPointF& corner : corners)
    {
        bounds |= QRectF(corner, corner);
    }
    return bounds.normalized();
}

/// Walks the /AP appearance streams of every annotation on \p page. The visitor
/// receives the resolved stream: PDFDocument::getObject() dereferences, so the
/// appearance object is never a reference and getReference() on it would throw.
void processAnnotationAppearanceStreams(PDFDocument* document,
                                        const PDFPage* page,
                                        int pageNumber,
                                        const std::function<void(const PDFPage*, const PDFStream*)>& processForm)
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
                        processForm(page, nested.getStream());
                    }
                }
            }
            else if (appearanceStream.isStream())
            {
                processForm(page, appearanceStream.getStream());
            }
        }
    }

    Q_UNUSED(pageNumber);
}

QString defaultFixupDescription(const QString& fixupId)
{
    if (fixupId == QStringLiteral("rgb-to-cmyk"))
    {
        return PDFTranslationContext::tr("Convert all RGB colors to CMYK");
    }
    if (fixupId == QStringLiteral("add-bleed"))
    {
        return PDFTranslationContext::tr("Extend page boxes / artwork to provide bleed");
    }
    if (fixupId == QStringLiteral("downsample-images"))
    {
        return PDFTranslationContext::tr("Downsample images above target DPI");
    }

    return PDFTranslationContext::tr("Apply fixup '%1'").arg(fixupId);
}

bool isBleedAdequate(const PDFPage* page, qreal amountPt, qreal tolerancePt, QRectF& bboxForReport)
{
    if (!page)
    {
        bboxForReport = QRectF();
        return false;
    }

    const QRectF media = page->getMediaBox().normalized();
    const QRectF trim = preflight::resolveEffectiveBox(page->getTrimBox(), page->getCropBox(), media);
    const QRectF bleed = page->getBleedBox().normalized();
    bboxForReport = bleed.isEmpty() ? media : bleed;

    return preflight::bleedAdequate(trim, bleed, amountPt, tolerancePt);
}

void runBleedCheck(PDFDocumentSession* session,
                   const PreflightCheckConfig& check,
                   QList<PreflightFinding>& errors,
                   QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    const PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        QRectF bbox;
        const bool adequate = isBleedAdequate(page, check.amountPt, check.tolerancePt, bbox);
        if (adequate)
        {
            continue;
        }

        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
        finding.page = int(pageIndex + 1);
        finding.objectId = QString();
        finding.type = QStringLiteral("bleed");
        finding.severity = check.severity;
        finding.checkId = check.id;
        finding.bbox = bbox;
        finding.message = PDFTranslationContext::tr("BleedBox is missing or less than %1 pt on one or more edges").arg(check.amountPt);

        if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
        {
            warnings.push_back(finding);
        }
        else
        {
            errors.push_back(finding);
        }
    }
}

enum class SizeCheckKind
{
    Trim,       ///< Measures the TrimBox (with trim -> crop -> media fallback).
    PageSize    ///< Measures the MediaBox (physical page size).
};

void runSizeCheck(SizeCheckKind kind,
                  PDFDocumentSession* session,
                  const PreflightCheckConfig& check,
                  QList<PreflightFinding>& errors,
                  QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    if (!check.hasExpectedSize)
    {
        return;
    }

    const PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const QString type = (kind == SizeCheckKind::Trim) ? QStringLiteral("trim") : QStringLiteral("page-size");
    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        const QRectF media = page->getMediaBox().normalized();
        const QRectF box = (kind == SizeCheckKind::Trim)
                               ? preflight::resolveEffectiveBox(page->getTrimBox(), page->getCropBox(), media)
                               : media;

        if (preflight::sizeWithinTolerance(box.width(), box.height(), check.expectedWidthPt, check.expectedHeightPt, check.tolerancePt))
        {
            continue;
        }

        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
        finding.page = int(pageIndex + 1);
        finding.objectId = QString();
        finding.type = type;
        finding.severity = check.severity;
        finding.checkId = check.id;
        finding.bbox = box;
        finding.message = (kind == SizeCheckKind::Trim)
                              ? PDFTranslationContext::tr("TrimBox %1 x %2 pt does not match expected %3 x %4 pt (tolerance %5 pt)")
                                    .arg(box.width()).arg(box.height()).arg(check.expectedWidthPt).arg(check.expectedHeightPt).arg(check.tolerancePt)
                              : PDFTranslationContext::tr("Page size %1 x %2 pt does not match expected %3 x %4 pt (tolerance %5 pt)")
                                    .arg(box.width()).arg(box.height()).arg(check.expectedWidthPt).arg(check.expectedHeightPt).arg(check.tolerancePt);

        if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
        {
            warnings.push_back(finding);
        }
        else
        {
            errors.push_back(finding);
        }
    }
}

QString sideNameForFinding(PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left: return QStringLiteral("left");
        case PDFBleedFixupSide::Right: return QStringLiteral("right");
        case PDFBleedFixupSide::Top: return QStringLiteral("top");
        case PDFBleedFixupSide::Bottom: return QStringLiteral("bottom");
    }
    return QStringLiteral("unknown");
}

void pushPreflightFinding(const PreflightFinding& finding,
                          const QString& severity,
                          QList<PreflightFinding>& errors,
                          QList<PreflightFinding>& warnings)
{
    if (severity == QStringLiteral("warning") || severity == QStringLiteral("info"))
    {
        warnings.push_back(finding);
    }
    else
    {
        errors.push_back(finding);
    }
}

bool edgeHasContent(const PDFBleedMarginProbeResult& result, PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left: return result.left.hasContent;
        case PDFBleedFixupSide::Right: return result.right.hasContent;
        case PDFBleedFixupSide::Top: return result.top.hasContent;
        case PDFBleedFixupSide::Bottom: return result.bottom.hasContent;
    }
    return false;
}

QRectF edgeStripRect(const PDFBleedMarginProbeResult& result, PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left: return result.left.stripRect;
        case PDFBleedFixupSide::Right: return result.right.stripRect;
        case PDFBleedFixupSide::Top: return result.top.stripRect;
        case PDFBleedFixupSide::Bottom: return result.bottom.stripRect;
    }
    return QRectF();
}

void emitNeedsAutoBleedFinding(int pageNumber,
                               const QRectF& pageBbox,
                               const PreflightCheckConfig& check,
                               QList<PreflightFinding>& errors,
                               QList<PreflightFinding>& warnings)
{
    PreflightFinding finding;
    finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
    finding.page = pageNumber;
    finding.objectId = QString();
    finding.type = QStringLiteral("needs-auto-bleed");
    finding.severity = QStringLiteral("info");
    finding.checkId = check.id;
    finding.bbox = pageBbox;
    finding.message = PDFTranslationContext::tr("Page is a candidate for the add-bleed fixup");
    pushPreflightFinding(finding, finding.severity, errors, warnings);
}

void runProcessingStepsCheck(PDFDocumentSession* session,
                             const PreflightCheckConfig& check,
                             QList<PreflightFinding>& errors,
                             QList<PreflightFinding>& warnings)
{
    if (!session || !session->getDocument())
    {
        return;
    }

    const QList<PDFProcessingStep> steps = detectProcessingSteps(*session->getDocument());
    const auto addFinding = [&check, &errors, &warnings](const QString& type,
                                                          const QString& message,
                                                          const PDFProcessingStep* step,
                                                          const QString& requiredType = QString())
    {
        PreflightFinding finding;
        finding.scope = step && step->pageIndices.size() == 1
                            ? QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE)
                            : QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.page = step && step->pageIndices.size() == 1 ? step->pageIndices.front() + 1 : 1;
        finding.type = type;
        finding.severity = check.severity;
        finding.checkId = check.id;
        finding.message = message;
        finding.bbox = step ? step->geometry.boundingRect() : QRectF();
        if (step)
        {
            finding.evidence = QJsonObject{
                { QStringLiteral("step_type"), pdfProcessingStepTypeToString(step->type) },
                { QStringLiteral("detection_method"), step->detectionMethod },
                { QStringLiteral("ocg_name"), step->ocgName },
                { QStringLiteral("spot_color"), step->spotColorName },
                { QStringLiteral("required_type"), requiredType },
                { QStringLiteral("is_separation"), step->isSeparation }
            };
        }
        pushPreflightFinding(finding, finding.severity, errors, warnings);
    };

    const auto isDieline = [](const PDFProcessingStep& step)
    {
        return step.type == PDFProcessingStepType::CuttingDie ||
               (step.isSeparation && step.detectionMethod == QStringLiteral("legacy-spot-color"));
    };

    if (check.required && std::none_of(steps.cbegin(), steps.cend(), isDieline))
    {
        addFinding(QStringLiteral("dieline-missing"),
                   PDFTranslationContext::tr("No ISO 19593-1 cutting-die OCG or legacy dieline spot color was detected."),
                   nullptr);
    }

    for (const QString& requiredType : check.requiredProcessingStepTypes)
    {
        const PDFProcessingStepType type = pdfProcessingStepTypeFromString(requiredType);
        const PDFProcessingStep* match = nullptr;
        for (const PDFProcessingStep& step : steps)
        {
            if (step.type == type)
            {
                match = &step;
                break;
            }
        }
        if (!match)
        {
            addFinding(QStringLiteral("processing-step-missing"),
                       PDFTranslationContext::tr("Required processing step '%1' was not detected.").arg(requiredType),
                       nullptr, requiredType);
        }
    }

    for (const PDFProcessingStep& step : steps)
    {
        if (isDieline(step) && step.shouldPrint)
        {
            addFinding(QStringLiteral("dieline-printing"),
                       PDFTranslationContext::tr("Dieline processing geometry is marked printable; it must be non-printing."),
                       &step);
        }
    }
}

void runContentBleedCheck(PDFDocumentSession* session,
                           const PreflightCheckConfig& check,
                           QList<PreflightFinding>& errors,
                           QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    PDFBleedMarginProbe probe(session);

    PDFBleedMarginProbeSettings probeSettings;
    const PDFReal bleedMm = convertPDFPointToMM(check.amountPt);
    probeSettings.bleedMM = QMarginsF(bleedMm, bleedMm, bleedMm, bleedMm);
    probeSettings.dpi = check.probeDpi;
    probeSettings.threshold = check.probeThreshold;
    probeSettings.whiteCoverageThreshold = check.rasterWhiteThreshold;
    probeSettings.fastOnly = !check.rasterConfirm;

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        const PDFBleedMarginProbeResult result = probe.probe(page, static_cast<size_t>(pageIndex), probeSettings);

        if (result.allEdgesCovered())
        {
            continue;
        }

        const PDFBleedFixupSide sides[4] = {
            PDFBleedFixupSide::Left, PDFBleedFixupSide::Right,
            PDFBleedFixupSide::Top, PDFBleedFixupSide::Bottom
        };

        bool pageHasBleedGap = false;

        if (check.rasterConfirm)
        {
            for (PDFBleedFixupSide side : sides)
            {
                if (edgeHasContent(result, side))
                {
                    continue;
                }

                const QRectF stripRect = edgeStripRect(result, side);
                PreflightFinding finding;
                finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
                finding.page = int(pageIndex + 1);
                finding.objectId = QString();
                finding.type = QStringLiteral("bleed-margin-empty");
                finding.severity = check.severity;
                finding.checkId = check.id;
                finding.bbox = stripRect.isValid() ? stripRect : QRectF();
                finding.message = PDFTranslationContext::tr("Bleed margin empty on %1 edge").arg(sideNameForFinding(side));
                pushPreflightFinding(finding, check.severity, errors, warnings);
                pageHasBleedGap = true;
            }
        }
        else
        {
            QStringList missingSides;
            QRectF unionMissingBbox;
            for (PDFBleedFixupSide side : sides)
            {
                if (edgeHasContent(result, side))
                {
                    continue;
                }

                missingSides.append(sideNameForFinding(side));
                const QRectF stripRect = edgeStripRect(result, side);
                if (stripRect.isValid())
                {
                    unionMissingBbox = unionMissingBbox.united(stripRect);
                }
            }

            if (missingSides.isEmpty())
            {
                continue;
            }

            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
            finding.page = int(pageIndex + 1);
            finding.objectId = QString();
            finding.type = QStringLiteral("content-bleed");
            finding.severity = check.severity;
            finding.checkId = check.id;
            finding.bbox = unionMissingBbox.isValid() ? unionMissingBbox : QRectF();
            finding.message = PDFTranslationContext::tr("Artwork does not extend into bleed margin on %1").arg(missingSides.join(QStringLiteral(", ")));
            pushPreflightFinding(finding, check.severity, errors, warnings);
            pageHasBleedGap = true;
        }

        if (pageHasBleedGap)
        {
            const QRectF media = page->getMediaBox().normalized();
            const QRectF pageBbox = preflight::resolveEffectiveBox(page->getTrimBox(), page->getCropBox(), media);
            emitNeedsAutoBleedFinding(int(pageIndex + 1), pageBbox, check, errors, warnings);
        }
    }
}

void runInkCoverageCheck(PDFDocumentSession* session,
                          const PreflightCheckConfig& check,
                          QList<PreflightFinding>& errors,
                          QList<PreflightFinding>& warnings)
{
    auto emitIncomplete = [&](int pageNumber, const QString& reason, bool budgetExceeded = false)
    {
        PreflightFinding finding;
        finding.scope = pageNumber > 0
            ? QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE)
            : QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.page = pageNumber;
        finding.type = QStringLiteral("check-incomplete");
        finding.severity = QStringLiteral("info");
        finding.checkId = check.id;
        finding.evidence = QJsonObject{
            { QStringLiteral("reason"), reason },
            { QStringLiteral("budget_exceeded"), budgetExceeded },
            { QStringLiteral("analysis_box"), check.inkCoverageAnalysisBox },
            { QStringLiteral("max_raster_pixels"), check.maxRasterPixels }
        };
        finding.message = pageNumber > 0
            ? PDFTranslationContext::tr("Page %1 skipped: %2").arg(pageNumber).arg(reason)
            : PDFTranslationContext::tr("Ink coverage skipped: %1").arg(reason);
        pushPreflightFinding(finding, finding.severity, errors, warnings);
    };

    if (!session)
    {
        emitIncomplete(0, PDFTranslationContext::tr("document session was unavailable"));
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        emitIncomplete(0, PDFTranslationContext::tr("document was unavailable"));
        return;
    }

    PDFInkCoverageProbeSettings probeSettings;
    probeSettings.maxInkCoverage = check.maxInkPct / 100.0;
    probeSettings.dpi = check.probeDpi;
    probeSettings.minRegionAreaRatio = check.minRegionAreaPct / 100.0;
    probeSettings.maxRegionsPerPage = check.maxRegionsPerPage;
    probeSettings.maxRasterPixels = check.maxRasterPixels;
    if (check.inkCoverageAnalysisBox == QStringLiteral("trim"))
    {
        probeSettings.analysisBox = PDFInkCoverageAnalysisBox::Trim;
    }
    else if (check.inkCoverageAnalysisBox == QStringLiteral("crop"))
    {
        probeSettings.analysisBox = PDFInkCoverageAnalysisBox::Crop;
    }
    else if (check.inkCoverageAnalysisBox == QStringLiteral("media"))
    {
        probeSettings.analysisBox = PDFInkCoverageAnalysisBox::Media;
    }

    PDFInkCoverageProbe probe(session);
    const PDFCatalog* catalog = document->getCatalog();
    if (!catalog)
    {
        emitIncomplete(0, PDFTranslationContext::tr("catalog was unavailable"));
        return;
    }
    const PDFInteger pageCount = catalog->getPageCount();

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            emitIncomplete(int(pageIndex + 1), PDFTranslationContext::tr("page could not be inspected"));
            continue;
        }

        const PDFInkCoverageProbeResult result = probe.probe(page, static_cast<size_t>(pageIndex), probeSettings);
        if (!result.rasterized)
        {
            emitIncomplete(int(pageIndex + 1),
                           result.budgetExceeded
                               ? PDFTranslationContext::tr("ink coverage raster exceeds the pixel budget")
                               : PDFTranslationContext::tr("ink coverage rasterization was unavailable"),
                           result.budgetExceeded);
            continue;
        }

        int regionRank = 0;
        for (const PDFInkCoverageRegion& region : result.regions)
        {
            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
            finding.page = int(pageIndex + 1);
            finding.objectId = QString();
            finding.type = QStringLiteral("ink-coverage");
            finding.severity = check.severity;
            finding.checkId = check.id;
            finding.bbox = region.bbox;
            finding.evidence = QJsonObject{
                { QStringLiteral("peak_ink_pct"), region.peakInkCoverage * 100.0 },
                { QStringLiteral("max_ink_pct"), check.maxInkPct },
                { QStringLiteral("area_mm2"), region.areaMM2 },
                { QStringLiteral("analysis_box"), check.inkCoverageAnalysisBox },
                { QStringLiteral("region_rank"), ++regionRank }
            };
            finding.message = PDFTranslationContext::tr(
                "Total ink coverage %1% exceeds maximum %2% over %3 mm^2 on page %4")
                .arg(qRound(region.peakInkCoverage * 100.0))
                .arg(qRound(check.maxInkPct))
                .arg(qRound(region.areaMM2))
                .arg(pageIndex + 1);
            pushPreflightFinding(finding, check.severity, errors, warnings);
        }
    }
}

/// Records a check that aborted before completing. The run continues with the
/// remaining checks, but the report is marked incomplete so pass can never be
/// true on a partial inspection.
void recordCheckFailure(PreflightResult& result,
                        PreflightCheckStatus& status,
                        const PreflightCheckConfig& check,
                        const QString& reason)
{
    status.status = QStringLiteral("failed");
    status.reason = reason;
    result.checkStatuses.push_back(status);
    result.inspectionComplete = false;
    if (result.errorCode.isEmpty())
    {
        result.errorCode = QStringLiteral("check-error");
        result.errorMessage = reason;
    }

    PreflightFinding finding;
    finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
    finding.type = QStringLiteral("check-error");
    finding.severity = QStringLiteral("error");
    finding.checkId = check.id;
    finding.bbox = QRectF();
    finding.message = PDFTranslationContext::tr("Check '%1' failed to run: %2").arg(check.id, reason);
    result.errors.push_back(finding);
}

void recordBudgetFailure(PreflightResult& result,
                         PreflightCheckStatus& status,
                         const PreflightCheckConfig& check,
                         const PDFBudgetExceededException& exception)
{
    const PDFBudgetExceeded& detail = exception.getDetail();
    status.status = QStringLiteral("incomplete");
    status.reason = QStringLiteral("budget-exceeded");
    status.budgetKind = QString::fromLatin1(getPDFBudgetKindName(detail.kind));
    status.budgetLimit = static_cast<qint64>(detail.limit);
    status.budgetAttempted = static_cast<qint64>(detail.attempted);
    status.budgetContext = detail.context;
    result.checkStatuses.push_back(status);
    result.inspectionComplete = false;

    PreflightFinding finding;
    finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
    finding.type = QStringLiteral("budget-exceeded");
    finding.severity = QStringLiteral("error");
    finding.checkId = check.id;
    finding.bbox = QRectF();
    finding.message = PDFTranslationContext::tr("Check '%1' exceeded the %2 processing budget (%3 > %4): %5")
        .arg(check.id, status.budgetKind)
        .arg(detail.attempted)
        .arg(detail.limit)
        .arg(detail.context);
    result.errors.push_back(finding);
}

bool hasBleedGapFinding(const QList<PreflightFinding>& findings)
{
    for (const PreflightFinding& finding : findings)
    {
        if (finding.checkId == QStringLiteral("bleed")
            || finding.type == QStringLiteral("content-bleed")
            || finding.type == QStringLiteral("bleed-margin-empty")
            || finding.type == QStringLiteral("needs-auto-bleed"))
        {
            return true;
        }
    }
    return false;
}

bool hasDownsampleCandidate(const PDFDocument* document,
                            int targetDpi,
                            int* candidateCount = nullptr)
{
    if (candidateCount)
    {
        *candidateCount = 0;
    }
    if (!document || targetDpi < 72 || targetDpi > 1200)
    {
        return false;
    }

    const std::vector<PDFImageOptimizer::ImageInfo> images = PDFImageOptimizer::collectImageInfos(document);
    constexpr double qualityThreshold = 1.15;
    int count = 0;
    for (const PDFImageOptimizer::ImageInfo& image : images)
    {
        if (image.isImageMask)
        {
            continue;
        }
        const double dpiX = image.minimalDpi.x();
        const double dpiY = image.minimalDpi.y();
        const bool highX = std::isfinite(dpiX) && dpiX > targetDpi * qualityThreshold;
        const bool highY = std::isfinite(dpiY) && dpiY > targetDpi * qualityThreshold;
        if (highX || highY)
        {
            ++count;
        }
    }

    if (candidateCount)
    {
        *candidateCount = count;
    }
    return count > 0;
}

void adjustFixupsAvailable(PDFDocumentSession* session,
                           QList<PreflightFixupConfig>& fixups,
                           bool needsAddBleed,
                           qreal addBleedAmountPt,
                           const QList<PreflightFinding>& errors,
                           const QList<PreflightFinding>& warnings)
{
    PreflightFixupConfig addBleedConfig;
    bool hasProfileAddBleed = false;
    PreflightFixupConfig rgbToCmykConfig;
    bool hasProfileRgbToCmyk = false;
    PreflightFixupConfig downsampleConfig;
    bool hasProfileDownsample = false;

    for (const PreflightFixupConfig& fixup : fixups)
    {
        if (fixup.id == QStringLiteral("add-bleed"))
        {
            addBleedConfig = fixup;
            hasProfileAddBleed = true;
            break;
        }
    }

    for (const PreflightFixupConfig& fixup : fixups)
    {
        if (fixup.id == QStringLiteral("downsample-images"))
        {
            downsampleConfig = fixup;
            hasProfileDownsample = true;
            break;
        }
    }

    for (const PreflightFixupConfig& fixup : fixups)
    {
        if (fixup.id == QStringLiteral("rgb-to-cmyk"))
        {
            rgbToCmykConfig = fixup;
            hasProfileRgbToCmyk = true;
            break;
        }
    }

    // Rebuild the list from the shared registry and document findings. Every
    // registered preflight fixup is finding-driven: clear the profile list after
    // capturing its parameters so a fixup is advertised only when this document
    // has the corresponding actionable finding.
    fixups.clear();

    if (needsAddBleed && isImplementedFixupId(QStringLiteral("add-bleed")))
    {
        if (!hasProfileAddBleed)
        {
            addBleedConfig.id = QStringLiteral("add-bleed");
            addBleedConfig.confirm = true;
        }
        if (addBleedConfig.description.isEmpty())
        {
            addBleedConfig.description = PDFTranslationContext::tr("Extend page boxes / artwork to provide bleed");
        }
        if (addBleedConfig.amountPt <= 0.0 && addBleedAmountPt > 0.0)
        {
            addBleedConfig.amountPt = addBleedAmountPt;
        }

        QJsonObject params = addBleedConfig.params;
        if (!params.contains(QStringLiteral("mode")))
        {
            params.insert(QStringLiteral("mode"), QStringLiteral("mirror"));
        }
        addBleedConfig.params = params;

        fixups.push_back(addBleedConfig);
    }

    const auto hasRgbFinding = [](const QList<PreflightFinding>& findings)
    {
        return std::any_of(findings.cbegin(), findings.cend(), [](const PreflightFinding& finding)
        {
            return finding.checkId == QStringLiteral("color-mode")
                && finding.message.contains(QStringLiteral("DeviceRGB"), Qt::CaseInsensitive);
        });
    };

    if ((hasRgbFinding(errors) || hasRgbFinding(warnings))
        && isImplementedFixupId(QStringLiteral("rgb-to-cmyk")))
    {
        if (!hasProfileRgbToCmyk)
        {
            rgbToCmykConfig.id = QStringLiteral("rgb-to-cmyk");
            rgbToCmykConfig.confirm = true;
        }
        if (rgbToCmykConfig.description.isEmpty())
        {
            rgbToCmykConfig.description = PDFTranslationContext::tr(
                "Convert RGB content to an ICC-managed CMYK output condition");
        }
        QJsonObject params = rgbToCmykConfig.params;
        params.insert(QStringLiteral("safe"), true);
        rgbToCmykConfig.params = params;
        fixups.push_back(rgbToCmykConfig);
    }

    const int targetDpi = downsampleConfig.params.value(QStringLiteral("target_dpi")).toInt(300);
    int candidateCount = 0;
    if (hasProfileDownsample
        && isImplementedFixupId(QStringLiteral("downsample-images"))
        && hasDownsampleCandidate(session ? session->getDocument() : nullptr, targetDpi, &candidateCount))
    {
        if (downsampleConfig.description.isEmpty())
        {
            downsampleConfig.description = PDFTranslationContext::tr(
                "Downsample %1 oversized image(s) toward %2 DPI")
                .arg(candidateCount)
                .arg(targetDpi);
        }

        QJsonObject params = downsampleConfig.params;
        params.insert(QStringLiteral("target_dpi"), targetDpi);
        params.insert(QStringLiteral("candidate_count"), candidateCount);
        params.insert(QStringLiteral("quality"), 90);
        params.insert(QStringLiteral("preserve_color"), true);
        params.insert(QStringLiteral("preserve_transparency"), true);
        downsampleConfig.params = params;
        fixups.push_back(downsampleConfig);
    }
}

void runColorModeCheck(PDFDocumentSession* session,
                       const PreflightCheckConfig& check,
                       QList<PreflightFinding>& errors,
                       QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    if (check.allowedColorModes.isEmpty())
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    QSet<QString> allowed;
    for (const QString& mode : check.allowedColorModes)
    {
        if (mode.compare(QStringLiteral("RGB"), Qt::CaseInsensitive) == 0)
        {
            allowed.insert(QStringLiteral("DeviceRGB"));
        }
        else if (mode.compare(QStringLiteral("CMYK"), Qt::CaseInsensitive) == 0)
        {
            allowed.insert(QStringLiteral("DeviceCMYK"));
        }
        else if (mode.compare(QStringLiteral("Grayscale"), Qt::CaseInsensitive) == 0)
        {
            allowed.insert(QStringLiteral("DeviceGray"));
        }
    }

    PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument md(document, &ocActivity);
    fontCache.setDocument(md);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFMeshQualitySettings meshQuality;

    class ColorModeProcessor : public PDFPageContentProcessor
    {
    public:
        ColorModeProcessor(const PDFPage* page,
                           const PDFDocument* doc,
                           const PDFFontCache* fc,
                           const PDFCMS* cms_p,
                           const PDFOptionalContentActivity* oc,
                           const PDFMeshQualitySettings& mq,
                           PDFProcessingBudget* budget,
                           QSet<QString>* paintedSpaces)
            : PDFPageContentProcessor(page, doc, fc, cms_p, oc, QTransform(), mq, budget)
            , m_paintedSpaces(paintedSpaces)
        {
        }

        void processFormStream(const PDFStream* stream)
        {
            if (!stream || isContentSuppressed())
            {
                return;
            }
            processForm(stream);
        }

    protected:
        bool isContentKindSuppressed(ContentKind kind) const override
        {
            switch (kind)
            {
                case ContentKind::Images:
                case ContentKind::Tiling:
                case ContentKind::Forms:
                case ContentKind::Shapes:
                case ContentKind::Text:
                    return false;
                default:
                    return true;
            }
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
            if (fill)
            {
                recordPaintedColorSpace(state->getFillColorSpace(), m_paintedSpaces);
            }
            if (stroke)
            {
                recordPaintedColorSpace(state->getStrokeColorSpace(), m_paintedSpaces);
            }
        }

        bool performOriginalImagePainting(const PDFImage& image,
                                           const PDFStream* stream,
                                           PDFObjectReference reference) override
        {
            Q_UNUSED(reference);
            if (isContentSuppressed())
            {
                return true;
            }

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
            return true;
        }

    private:
        QSet<QString>* m_paintedSpaces;
    };

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        QSet<QString> paintedSpaces;
        std::set<PDFObjectReference> visitedForms;
        collectColorSpacesFromResources(document, page->getResources(), &paintedSpaces, visitedForms, 0);

        ColorModeProcessor processor(page, document, &fontCache, cms.get(), &ocActivity, meshQuality, session->getProcessingBudget(), &paintedSpaces);
        processor.processContents();

        processAnnotationAppearanceStreams(document, page, int(pageIndex + 1), [&](const PDFPage* /*pageRef*/, const PDFStream* formStream) {
            processor.processFormStream(formStream);
        });

        QStringList disallowed;
        for (const QString& cs : paintedSpaces)
        {
            if (!allowed.contains(cs))
            {
                disallowed.append(cs);
            }
        }

        // paintedSpaces is a QSet: iteration order is unspecified and varies per
        // process. Sort so identical input always yields an identical report.
        disallowed.sort();

        if (!disallowed.isEmpty())
        {
            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
            finding.page = int(pageIndex + 1);
            finding.type = QStringLiteral("color-mode");
            finding.severity = check.severity;
            finding.checkId = check.id;
            finding.bbox = QRectF();
            QString modeList;
            for (const QString& mode : check.allowedColorModes)
            {
                if (!modeList.isEmpty())
                {
                    modeList += QStringLiteral(", ");
                }
                modeList += mode;
            }
            finding.message = PDFTranslationContext::tr(
                "Disallowed color space(s) found on page %1: %2 (allowed: %3)")
                .arg(pageIndex + 1)
                .arg(disallowed.join(QStringLiteral(", ")))
                .arg(modeList);

            if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
            {
                warnings.push_back(finding);
            }
            else
            {
                errors.push_back(finding);
            }
        }
    }
}

void runColorInventoryCheck(PDFDocumentSession* session,
                            const PreflightCheckConfig& check,
                            QList<PreflightFinding>& errors,
                            QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    PDFColorInventorySettings settings;
    settings.probeDpi = check.colorProbeDpi;
    settings.richBlackKThreshold = check.richBlackKThreshold;

    PDFColorInventory inventory(session);
    const PDFColorInventoryResult result = inventory.inspect(settings);

    auto emitInfo = [&](PreflightFinding finding)
    {
        finding.severity = check.severity;
        finding.checkId = check.id;
        pushPreflightFinding(finding, finding.severity, errors, warnings);
    };

    for (const PDFColorInventoryInk& ink : result.spotColors)
    {
        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.objectId = ink.name;
        finding.type = QStringLiteral("spot-color");
        finding.message = PDFTranslationContext::tr("Spot color detected: %1").arg(ink.name);
        emitInfo(finding);
    }

    for (const PDFColorInventoryInk& ink : result.separations)
    {
        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.objectId = ink.name;
        finding.type = QStringLiteral("separation");
        finding.message = ink.isSpot
                ? PDFTranslationContext::tr("Spot output separation: %1").arg(ink.name)
                : PDFTranslationContext::tr("Process output separation: %1").arg(ink.name);
        emitInfo(finding);
    }

    for (const PDFRichBlackInventory& richBlack : result.richBlackPages)
    {
        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
        finding.page = richBlack.page;
        finding.type = QStringLiteral("rich-black");
        finding.message = PDFTranslationContext::tr(
            "Rich black detected on page %1 (approximately %2 mm²; K > %3%).")
                .arg(richBlack.page)
                .arg(richBlack.areaMM2, 0, 'f', 2)
                .arg(check.richBlackKThreshold * 100.0, 0, 'f', 0);
        emitInfo(finding);
    }
}

/// Owns a cmsHPROFILE for the duration of a scope. The output-intent check has
/// several early-exit paths; a guard keeps them from leaking the handle.
class IccProfileGuard
{
public:
    explicit IccProfileGuard(const QByteArray& data) :
        m_profile(cmsOpenProfileFromMem(data.data(), cmsUInt32Number(data.size())))
    {
    }

    ~IccProfileGuard()
    {
        if (m_profile)
        {
            cmsCloseProfile(m_profile);
        }
    }

    IccProfileGuard(const IccProfileGuard&) = delete;
    IccProfileGuard& operator=(const IccProfileGuard&) = delete;

    bool isValid() const { return m_profile != nullptr; }
    cmsColorSpaceSignature getColorSpace() const { return cmsGetColorSpace(m_profile); }

private:
    cmsHPROFILE m_profile;
};

QString classifyIccColorSpace(cmsColorSpaceSignature signature)
{
    switch (signature)
    {
        case cmsSigGrayData:
            return QStringLiteral("Grayscale");
        case cmsSigRgbData:
            return QStringLiteral("RGB");
        case cmsSigCmykData:
            return QStringLiteral("CMYK");
        default:
            return QString();
    }
}

void runOutputIntentCheck(PDFDocumentSession* session,
                          const PreflightCheckConfig& check,
                          QList<PreflightFinding>& errors,
                          QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const auto& outputIntents = catalog->getOutputIntents();
    auto recordFinding = [&](const QString& type, const QString& message) {
        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.type = type;
        finding.severity = check.severity;
        finding.checkId = check.id;
        finding.message = message;

        if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
        {
            warnings.push_back(finding);
        }
        else
        {
            errors.push_back(finding);
        }
    };

    if (catalog->hasMalformedOutputIntents())
    {
        recordFinding(
            QStringLiteral("output-intent-malformed"),
            PDFTranslationContext::tr("The catalog /OutputIntents array contains a non-dictionary or unresolved entry."));
    }

    if (outputIntents.empty())
    {
        if (check.required)
        {
            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
            finding.type = QStringLiteral("output-intent-missing");
            finding.severity = check.severity;
            finding.checkId = check.id;
            finding.message = PDFTranslationContext::tr(
                "No output intent is defined; the intended printing condition cannot be determined.");

            if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
            {
                warnings.push_back(finding);
            }
            else
            {
                errors.push_back(finding);
            }
        }
        return;
    }

    QSet<QString> resolvedColorSpaces;
    QStringList validIntentLabels;
    for (size_t intentIndex = 0; intentIndex < outputIntents.size(); ++intentIndex)
    {
        const PDFOutputIntent& outputIntent = outputIntents[intentIndex];
        const QString identifier = outputIntent.getOutputConditionIdentifier();
        const QString label = identifier.isEmpty() ? QStringLiteral("(unnamed)") : identifier;
        const QString indexedLabel = outputIntents.size() > 1
            ? PDFTranslationContext::tr("intent %1 '%2'").arg(int(intentIndex)).arg(label)
            : label;

        if (!check.allowedOutputIntentSubtypes.isEmpty()
            && std::none_of(check.allowedOutputIntentSubtypes.cbegin(), check.allowedOutputIntentSubtypes.cend(), [&outputIntent](const QString& subtype) {
                return subtype.compare(QString::fromLatin1(outputIntent.getSubtype()), Qt::CaseInsensitive) == 0;
            }))
        {
            recordFinding(
                QStringLiteral("output-intent-subtype"),
                PDFTranslationContext::tr("Output intent %1 has subtype '%2', which is not allowed (allowed: %3).")
                    .arg(indexedLabel, QString::fromLatin1(outputIntent.getSubtype()), check.allowedOutputIntentSubtypes.join(QStringLiteral(", "))));
        }

        if (identifier.isEmpty())
        {
            recordFinding(
                QStringLiteral("output-intent-identity"),
                PDFTranslationContext::tr(
                    "Output intent has no /OutputConditionIdentifier, so the intended printing condition cannot be identified."));
        }
        else if (!check.allowedOutputConditionIdentifiers.isEmpty() &&
                 !check.allowedOutputConditionIdentifiers.contains(identifier))
        {
            recordFinding(
                QStringLiteral("output-intent-identity"),
                PDFTranslationContext::tr(
                    "Output intent condition identifier '%1' is not allowed (allowed: %2).")
                    .arg(identifier, check.allowedOutputConditionIdentifiers.join(QStringLiteral(", "))));
        }

        const PDFObject outputProfileObject = document->getObject(outputIntent.getOutputProfile());
        if (!outputProfileObject.isStream())
        {
            if (check.requireEmbeddedOutputIntentProfile)
            {
                recordFinding(
                    QStringLiteral("output-intent-profile-missing"),
                    PDFTranslationContext::tr(
                        "Output intent '%1' has no embedded ICC profile (/DestOutputProfile is missing or is not a stream).")
                        .arg(indexedLabel));
            }
            continue;
        }

        QByteArray content;
        try
        {
            const PDFObject& outputProfileReference = outputIntent.getOutputProfile();
            if (outputProfileReference.isReference())
            {
                content = session->getDecodedStream(outputProfileReference.getReference());
            }
            else
            {
                content = document->getDecodedStream(outputProfileObject.getStream(), session->getProcessingBudget());
            }
        }
        catch (const PDFException&)
        {
            recordFinding(
                QStringLiteral("output-intent-profile-invalid"),
                PDFTranslationContext::tr(
                    "Output intent '%1' has an ICC profile that could not be decoded.")
                    .arg(indexedLabel));
            continue;
        }

        IccProfileGuard profile(content);
        if (!profile.isValid())
        {
            recordFinding(
                QStringLiteral("output-intent-profile-invalid"),
                PDFTranslationContext::tr(
                    "Output intent '%1' has an ICC profile that is not a valid ICC profile.")
                    .arg(indexedLabel));
            continue;
        }

        const QString colorSpace = classifyIccColorSpace(profile.getColorSpace());
        if (colorSpace.isEmpty())
        {
            recordFinding(
                QStringLiteral("output-intent-profile-invalid"),
                PDFTranslationContext::tr(
                    "Output intent '%1' has an ICC profile with an unsupported color space.")
                    .arg(indexedLabel));
            continue;
        }

        resolvedColorSpaces.insert(colorSpace);
        validIntentLabels.append(indexedLabel);

        if (!check.allowedOutputIntentProfileSha256.isEmpty())
        {
            const QString profileSha256 = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
            if (std::none_of(check.allowedOutputIntentProfileSha256.cbegin(), check.allowedOutputIntentProfileSha256.cend(), [&profileSha256](const QString& allowed) {
                return allowed.compare(profileSha256, Qt::CaseInsensitive) == 0;
            }))
            {
                recordFinding(
                    QStringLiteral("output-intent-profile-identity"),
                    PDFTranslationContext::tr("Output intent %1 has ICC payload identity %2, which is not allowed.")
                        .arg(indexedLabel, profileSha256));
            }
        }

        const QString declaredColorSpace = QString::fromLatin1(outputIntent.getOutputProfileInfo().getSignature());
        if (!declaredColorSpace.isEmpty() && declaredColorSpace.compare(colorSpace, Qt::CaseInsensitive) != 0)
        {
            recordFinding(
                QStringLiteral("output-intent-color-mismatch"),
                PDFTranslationContext::tr(
                    "Output intent '%1' declares ProfileCS '%2' but its embedded ICC profile is %3.")
                    .arg(indexedLabel, declaredColorSpace, colorSpace));
        }

        if (!check.allowedColorModes.isEmpty())
        {
            bool allowed = false;
            for (const QString& mode : check.allowedColorModes)
            {
                if (mode.compare(colorSpace, Qt::CaseInsensitive) == 0)
                {
                    allowed = true;
                    break;
                }
            }

            if (!allowed)
            {
                recordFinding(
                    QStringLiteral("output-intent-color-mismatch"),
                    PDFTranslationContext::tr(
                        "Output intent '%1' ICC profile color space %2 is not allowed (allowed: %3).")
                        .arg(indexedLabel, colorSpace, check.allowedColorModes.join(QStringLiteral(", "))));
            }
        }
    }

    if (!check.allowMultipleOutputIntents && validIntentLabels.size() > 1)
    {
        recordFinding(
            QStringLiteral("output-intent-ambiguous"),
            PDFTranslationContext::tr("Multiple valid applicable output intents were found (%1); policy requires a unique target.")
                .arg(validIntentLabels.join(QStringLiteral(", "))));
    }

    QStringList conflictColorSpaces = resolvedColorSpaces.values();
    conflictColorSpaces.sort();
    if (conflictColorSpaces.size() > 1)
    {
        recordFinding(
            QStringLiteral("output-intent-conflict"),
            PDFTranslationContext::tr(
                "Document defines %1 output intents with conflicting color spaces: %2.")
                .arg(int(outputIntents.size()))
                .arg(conflictColorSpaces.join(QStringLiteral(", "))));
    }
}

bool isNearWhiteDevicePaint(const PDFAbstractColorSpace* colorSpace, const PDFColor& color)
{
    if (!colorSpace)
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
        default:
            return false;
    }
}

void runWhiteOverprintCheck(PDFDocumentSession* session,
                            const PreflightCheckConfig& check,
                            QList<PreflightFinding>& errors,
                            QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument md(document, &ocActivity);
    fontCache.setDocument(md);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFMeshQualitySettings meshQuality;

    class WhiteOverprintProcessor : public PDFPageContentProcessor
    {
    public:
        WhiteOverprintProcessor(const PDFPage* page,
                                const PDFDocument* doc,
                                const PDFFontCache* fc,
                                const PDFCMS* cms_p,
                                const PDFOptionalContentActivity* oc,
                                const PDFMeshQualitySettings& mq,
                                PDFProcessingBudget* budget,
                                bool* foundWhiteOverprint)
            : PDFPageContentProcessor(page, doc, fc, cms_p, oc, QTransform(), mq, budget)
            , m_foundWhiteOverprint(foundWhiteOverprint)
        {
        }

        void processFormStream(const PDFStream* stream)
        {
            if (!stream || isContentSuppressed())
            {
                return;
            }
            processForm(stream);
        }

    protected:
        bool isContentKindSuppressed(ContentKind kind) const override
        {
            switch (kind)
            {
                case ContentKind::Shapes:
                case ContentKind::Text:
                case ContentKind::Forms:
                    return false;
                default:
                    return true;
            }
        }

        void performPathPainting(const QPainterPath& path, bool stroke, bool fill, bool text, Qt::FillRule fillRule) override
        {
            Q_UNUSED(path);
            Q_UNUSED(text);
            Q_UNUSED(fillRule);

            if (isContentSuppressed() || m_foundWhiteOverprint == nullptr)
            {
                return;
            }

            const PDFPageContentProcessorState* state = getGraphicState();
            const PDFOverprintMode overprintMode = state->getOverprintMode();

            if (fill && overprintMode.overprintFilling
                && isNearWhiteDevicePaint(state->getFillColorSpace(), state->getFillColorOriginal()))
            {
                *m_foundWhiteOverprint = true;
            }

            if (stroke && overprintMode.overprintStroking
                && isNearWhiteDevicePaint(state->getStrokeColorSpace(), state->getStrokeColorOriginal()))
            {
                *m_foundWhiteOverprint = true;
            }
        }

    private:
        bool* m_foundWhiteOverprint = nullptr;
    };

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        bool foundWhiteOverprint = false;
        WhiteOverprintProcessor processor(page, document, &fontCache, cms.get(), &ocActivity, meshQuality, session->getProcessingBudget(), &foundWhiteOverprint);
        processor.processContents();

        processAnnotationAppearanceStreams(document, page, int(pageIndex + 1), [&](const PDFPage* /*pageRef*/, const PDFStream* formStream) {
            processor.processFormStream(formStream);
        });

        if (!foundWhiteOverprint)
        {
            continue;
        }

        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
        finding.page = int(pageIndex + 1);
        finding.type = QStringLiteral("white-overprint");
        finding.severity = check.severity;
        finding.checkId = check.id;
        finding.message = PDFTranslationContext::tr(
            "White or near-white paint is set to overprint on page %1.")
                              .arg(pageIndex + 1);

        if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
        {
            warnings.push_back(finding);
        }
        else
        {
            errors.push_back(finding);
        }
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
        case TransparencyColorFamily::Unknown:
            break;
    }

    return QStringLiteral("Unknown");
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
        case PDFAbstractColorSpace::ColorSpace::ICCBased:
            return TransparencyColorFamily::RGB;

        case PDFAbstractColorSpace::ColorSpace::DeviceCMYK:
            return TransparencyColorFamily::CMYK;

        case PDFAbstractColorSpace::ColorSpace::Separation:
        case PDFAbstractColorSpace::ColorSpace::DeviceN:
            return TransparencyColorFamily::Spot;

        default:
            return TransparencyColorFamily::Unknown;
    }
}

bool isRiskyTransparencyConversion(TransparencyColorFamily blendSpace,
                                   TransparencyColorFamily sourceSpace)
{
    if (blendSpace == TransparencyColorFamily::Unknown
        || sourceSpace == TransparencyColorFamily::Unknown
        || blendSpace == sourceSpace)
    {
        return false;
    }

    // Gray content can enter a process-color transparency group without being
    // treated as a production risk by this check.
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

    return (blendSpace == TransparencyColorFamily::RGB && sourceSpace == TransparencyColorFamily::CMYK)
        || (blendSpace == TransparencyColorFamily::CMYK && sourceSpace == TransparencyColorFamily::RGB);
}

struct TransparencyGroupFrame
{
    TransparencyColorFamily blendSpace = TransparencyColorFamily::Unknown;
    bool hasExplicitBlendSpace = false;
    BlendMode entryBlendMode = BlendMode::Normal;
    QSet<TransparencyColorFamily> paintedSpaces;
};

class TransparencyRiskProcessor final : public PDFPageContentProcessor
{
public:
    TransparencyRiskProcessor(const PDFPage* page,
                              const PDFDocument* document,
                              const PDFFontCache* fontCache,
                              const PDFCMS* cms,
                              const PDFOptionalContentActivity* optionalContentActivity,
                              const PDFMeshQualitySettings& meshQuality,
                              PDFProcessingBudget* budget,
                              QSet<QString>* riskyBlendModes,
                              QSet<QString>* mismatchDescriptions) :
        PDFPageContentProcessor(page, document, fontCache, cms, optionalContentActivity, QTransform(), meshQuality, budget),
        m_riskyBlendModes(riskyBlendModes),
        m_mismatchDescriptions(mismatchDescriptions)
    {
    }

    void processFormStream(const PDFStream* stream)
    {
        if (stream && !isContentSuppressed())
        {
            processForm(stream);
        }
    }

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        Q_UNUSED(kind);
        return false;
    }

    void performBeginTransparencyGroup(ProcessOrder order, const PDFTransparencyGroup& group) override
    {
        if (order != ProcessOrder::BeforeOperation)
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

        if (order != ProcessOrder::AfterOperation || m_groups.empty())
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
        if (state.getStateFlags().testFlag(PDFPageContentProcessorState::StateBlendMode))
        {
            inspectBlendMode(state.getBlendMode());
        }
        PDFPageContentProcessor::performUpdateGraphicsState(state);
    }

    void performPathPainting(const QPainterPath& path,
                             bool stroke,
                             bool fill,
                             bool text,
                             Qt::FillRule fillRule) override
    {
        Q_UNUSED(path);
        Q_UNUSED(text);
        Q_UNUSED(fillRule);

        if (isContentSuppressed() || m_groups.empty())
        {
            return;
        }

        const PDFPageContentProcessorState* state = getGraphicState();
        if (fill)
        {
            recordPaintedSpace(state->getFillColorSpace());
        }
        if (stroke)
        {
            recordPaintedSpace(state->getStrokeColorSpace());
        }
    }

    bool performOriginalImagePainting(const PDFImage& image,
                                      const PDFStream* stream,
                                      PDFObjectReference reference) override
    {
        Q_UNUSED(stream);
        Q_UNUSED(reference);
        if (!isContentSuppressed())
        {
            recordPaintedImageSpace(image);
        }
        return true;
    }

    bool performPathPaintingUsingShading(const QPainterPath& path,
                                         bool stroke,
                                         bool fill,
                                         const PDFShadingPattern* shadingPattern) override
    {
        Q_UNUSED(path);
        Q_UNUSED(stroke);
        Q_UNUSED(fill);
        if (shadingPattern && !isContentSuppressed())
        {
            recordPaintedSpace(shadingPattern->getColorSpace());
        }
        return false;
    }

private:
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
        if (!frame.hasExplicitBlendSpace
            || frame.blendSpace == TransparencyColorFamily::Unknown
            || !m_mismatchDescriptions)
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

    std::vector<TransparencyGroupFrame> m_groups;
    QSet<QString>* m_riskyBlendModes = nullptr;
    QSet<QString>* m_mismatchDescriptions = nullptr;
};

void runTransparencyRiskCheck(PDFDocumentSession* session,
                              const PreflightCheckConfig& check,
                              QList<PreflightFinding>& errors,
                              QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument modifiedDocument(document, &ocActivity);
    fontCache.setDocument(modifiedDocument);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFMeshQualitySettings meshQuality;

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        QSet<QString> riskyBlendModes;
        QSet<QString> mismatchDescriptions;
        TransparencyRiskProcessor processor(page,
                                             document,
                                             &fontCache,
                                             cms.get(),
                                             &ocActivity,
                                             meshQuality,
                                             session->getProcessingBudget(),
                                             &riskyBlendModes,
                                             &mismatchDescriptions);

        processor.processContents();
        processAnnotationAppearanceStreams(document, page, int(pageIndex + 1), [&](const PDFPage* /*pageRef*/, const PDFStream* formStream) {
            processor.processFormStream(formStream);
        });

        QStringList blendModes = riskyBlendModes.values();
        blendModes.sort();
        if (!blendModes.isEmpty())
        {
            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
            finding.page = int(pageIndex + 1);
            finding.type = QStringLiteral("transparency-blend-mode");
            finding.severity = check.severity;
            finding.checkId = check.id;
            finding.message = PDFTranslationContext::tr(
                "Transparency uses blend mode configuration(s) that may not be reproduced reliably by all render paths: %1")
                .arg(blendModes.join(QStringLiteral(", ")));
            pushPreflightFinding(finding, check.severity, errors, warnings);
        }

        QStringList mismatches = mismatchDescriptions.values();
        mismatches.sort();
        if (!mismatches.isEmpty())
        {
            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_PAGE);
            finding.page = int(pageIndex + 1);
            finding.type = QStringLiteral("transparency-blend-space");
            finding.severity = check.severity;
            finding.checkId = check.id;
            finding.message = PDFTranslationContext::tr("Potential transparency blend-space mismatch: %1")
                .arg(mismatches.join(QStringLiteral("; ")));
            pushPreflightFinding(finding, check.severity, errors, warnings);
        }
    }
}

struct ThinStrokeFinding
{
    QString type;
    QRectF bbox;
    qreal declaredWidth = 0.0;
    qreal effectiveWidth = 0.0;
};

class ThinStrokeProcessor : public PDFPageContentProcessor
{
public:
    ThinStrokeProcessor(const PDFPage* page,
                        const PDFDocument* document,
                        const PDFFontCache* fontCache,
                        const PDFCMS* cms,
                        const PDFOptionalContentActivity* optionalContentActivity,
                        const PDFMeshQualitySettings& meshQualitySettings,
                        PDFProcessingBudget* budget,
                        qreal minimumWidth,
                        qreal zeroWidthEpsilon) :
        PDFPageContentProcessor(page, document, fontCache, cms, optionalContentActivity, QTransform(), meshQualitySettings, budget),
        m_minimumWidth(minimumWidth),
        m_zeroWidthEpsilon(zeroWidthEpsilon)
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

    const QList<ThinStrokeFinding>& findings() const { return m_findings; }
    const QList<PDFRenderError>& renderErrors() const { return getRenderErrors(); }

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        switch (kind)
        {
            case ContentKind::Shapes:
            case ContentKind::Text:
            case ContentKind::Forms:
                return false;
            default:
                return true;
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

        if (!stroke || m_minimumWidth <= 0.0 || path.isEmpty())
        {
            return;
        }

        const PDFPageContentProcessorState* state = getGraphicState();
        const qreal declaredWidth = state->getLineWidth();
        const bool hairline = declaredWidth <= m_zeroWidthEpsilon;
        const qreal effectiveWidth = preflight::minimumEffectiveStrokeWidth(
            path,
            declaredWidth,
            state->getCurrentTransformationMatrix(),
            getPage() ? getPage()->getUserUnit() : 1.0);
        if (!hairline && !(effectiveWidth < m_minimumWidth))
        {
            return;
        }

        QPainterPathStroker stroker;
        stroker.setWidth(std::max(std::abs(declaredWidth), m_zeroWidthEpsilon));
        stroker.setCapStyle(state->getLineCapStyle());
        stroker.setJoinStyle(state->getLineJoinStyle());
        stroker.setMiterLimit(state->getMitterLimit());
        const PDFLineDashPattern& dash = state->getLineDashPattern();
        if (!dash.isSolid())
        {
            stroker.setDashPattern(dash.createForQPen(std::max(std::abs(declaredWidth), m_zeroWidthEpsilon)));
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

        ThinStrokeFinding finding;
        finding.type = hairline ? QStringLiteral("hairline-stroke") : QStringLiteral("thin-stroke");
        finding.bbox = visibleStroke.boundingRect();
        finding.declaredWidth = declaredWidth;
        finding.effectiveWidth = effectiveWidth;
        m_findings.push_back(finding);
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

private:
    qreal m_minimumWidth = 0.0;
    qreal m_zeroWidthEpsilon = 1.0e-6;
    QPainterPath m_clipPath;
    std::vector<QPainterPath> m_clipStack;
    QList<ThinStrokeFinding> m_findings;
};

void throwIfThinStrokeProcessingIncomplete(const QList<PDFRenderError>& errors)
{
    for (const PDFRenderError& error : errors)
    {
        if (error.type == RenderErrorType::Error
            || error.type == RenderErrorType::NotImplemented
            || error.type == RenderErrorType::NotSupported)
        {
            throw PDFException(error.message);
        }
    }
}

void runThinStrokesCheck(PDFDocumentSession* session,
                         const PreflightCheckConfig& check,
                         QList<PreflightFinding>& errors,
                         QList<PreflightFinding>& warnings)
{
    if (!session || check.minEffectiveStrokeWidthPt <= 0.0)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument md(document, &ocActivity);
    fontCache.setDocument(md);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFMeshQualitySettings meshQuality;

    const QString hairlineSeverity = check.hairlineSeverity.isEmpty() ? check.severity : check.hairlineSeverity;
    const QString thinStrokeSeverity = check.thinStrokeSeverity.isEmpty() ? check.severity : check.thinStrokeSeverity;

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        ThinStrokeProcessor processor(page,
                                      document,
                                      &fontCache,
                                      cms.get(),
                                      &ocActivity,
                                      meshQuality,
                                      session->getProcessingBudget(),
                                      check.minEffectiveStrokeWidthPt,
                                      check.zeroWidthEpsilonPt);
        const QList<PDFRenderError> pageErrors = processor.processContents();
        throwIfThinStrokeProcessingIncomplete(pageErrors);

        processAnnotationAppearanceStreams(document, page, int(pageIndex + 1), [&](const PDFPage* /*pageRef*/, const PDFStream* formStream) {
            processor.processFormStream(formStream);
        });
        throwIfThinStrokeProcessingIncomplete(processor.renderErrors());

        for (const ThinStrokeFinding& source : processor.findings())
        {
            const bool hairline = source.type == QStringLiteral("hairline-stroke");
            const QString severity = hairline ? hairlineSeverity : thinStrokeSeverity;

            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
            finding.page = int(pageIndex + 1);
            finding.objectId = QString();
            finding.type = source.type;
            finding.severity = severity;
            finding.checkId = check.id;
            finding.bbox = source.bbox;
            if (hairline)
            {
                finding.message = PDFTranslationContext::tr(
                    "Hairline stroke on page %1 has declared width %2 pt.")
                    .arg(pageIndex + 1)
                    .arg(source.declaredWidth, 0, 'f', 6);
            }
            else
            {
                finding.message = PDFTranslationContext::tr(
                    "Thin stroke on page %1 has minimum effective width %2 pt below %3 pt.")
                    .arg(pageIndex + 1)
                    .arg(source.effectiveWidth, 0, 'f', 6)
                    .arg(check.minEffectiveStrokeWidthPt, 0, 'f', 6);
            }
            pushPreflightFinding(finding, severity, errors, warnings);
        }
    }
}

struct HiddenContentFinding
{
    QString type;
    QRectF bbox;
    QString detail;
    bool heuristic = false;
};

class HiddenContentProcessor final : public PDFPageContentProcessor
{
public:
    HiddenContentProcessor(const PDFPage* page,
                           const PDFDocument* document,
                           const PDFFontCache* fontCache,
                           const PDFCMS* cms,
                           const PDFOptionalContentActivity* optionalContentActivity,
                           const PDFMeshQualitySettings& meshQualitySettings,
                           PDFProcessingBudget* budget,
                           qreal offPageAllowance) :
        PDFPageContentProcessor(page,
                                 document,
                                 fontCache,
                                 cms,
                                 optionalContentActivity,
                                 QTransform(),
                                 meshQualitySettings,
                                 budget)
    {
        if (page)
        {
            const QRectF media = page->getMediaBox().normalized();
            const QRectF effective = preflight::resolveEffectiveBox(page->getTrimBox(), page->getCropBox(), media);
            const QRectF bleed = page->getBleedBox().normalized();
            m_toleratedBounds = bleed.isEmpty()
                ? effective.adjusted(-offPageAllowance, -offPageAllowance, offPageAllowance, offPageAllowance)
                : bleed;
        }
    }

    const QList<HiddenContentFinding>& findings() const { return m_findings; }

protected:
    void performMarkedContentBegin(const QByteArray& tag, const PDFObject& properties) override
    {
        if (tag != "OC" || !isContentSuppressed())
        {
            return;
        }

        QString name = QStringLiteral("unnamed optional-content group");
        PDFObjectReference reference;
        if (properties.isName() && getPropertiesDictionary())
        {
            const PDFObject property = getPropertiesDictionary()->get(properties.getString());
            if (property.isReference())
            {
                reference = property.getReference();
            }
        }
        if (reference.isValid() && getDocument()->getCatalog()->getOptionalContentProperties()->hasOptionalContentGroup(reference))
        {
            name = getDocument()->getCatalog()->getOptionalContentProperties()
                ->getOptionalContentGroup(reference).getName();
            if (name.isEmpty())
            {
                name = QStringLiteral("object %1 %2").arg(reference.objectNumber).arg(reference.generation);
            }
        }
        if (!m_hiddenLayers.contains(name))
        {
            m_hiddenLayers.append(name);
            m_findings.append({ QStringLiteral("hidden-layers"), QRectF(), name, false });
        }
    }

    void performInterceptInstruction(Operator currentOperator,
                                     ProcessOrder processOrder,
                                     const QByteArray& operatorAsText) override
    {
        if (processOrder != ProcessOrder::BeforeOperation
            || getGraphicState()->getTextRenderingMode() != TextRenderingMode::Invisible)
        {
            return;
        }

        switch (currentOperator)
        {
            case Operator::TextShowTextString:
            case Operator::TextShowTextIndividualSpacing:
            case Operator::TextNextLineShowText:
            case Operator::TextSetSpacingAndShowText:
                m_findings.append({ QStringLiteral("invisible-content"), QRectF(),
                                    QStringLiteral("text render mode 3 (%1)").arg(QString::fromLatin1(operatorAsText)), false });
                break;
            default:
                break;
        }
    }

    void performBeforePathPainting(const QPainterPath& path,
                                   bool stroke,
                                   bool fill,
                                   bool text,
                                   Qt::FillRule fillRule) override
    {
        Q_UNUSED(fillRule);
        if (path.isEmpty() || (!stroke && !fill && !text))
        {
            return;
        }

        const QRectF bounds = getCurrentWorldMatrix().map(path).boundingRect().normalized();
        const PDFPageContentProcessorState* state = getGraphicState();
        if (state->getAlphaFilling() <= 0.0 || state->getAlphaStroking() <= 0.0)
        {
            m_findings.append({ QStringLiteral("invisible-content"), bounds,
                                QStringLiteral("graphics-state alpha is zero"), false });
        }

        if (!m_toleratedBounds.isEmpty() && !m_toleratedBounds.intersects(bounds))
        {
            m_findings.append({ QStringLiteral("off-page-content"), bounds,
                                QStringLiteral("mark lies outside the effective page/bleed box"), false });
        }

        if (fill && state->getAlphaFilling() >= 1.0 && !m_paintedBounds.isEmpty())
        {
            for (const QRectF& previous : m_paintedBounds)
            {
                if (bounds.contains(previous))
                {
                    m_findings.append({ QStringLiteral("obscured-content"), previous,
                                        QStringLiteral("fully covered by later opaque paint"), true });
                    break;
                }
            }
        }
        if (fill || stroke || text)
        {
            m_paintedBounds.append(bounds);
        }
    }

private:
    QRectF m_toleratedBounds;
    QList<QRectF> m_paintedBounds;
    QStringList m_hiddenLayers;
    QList<HiddenContentFinding> m_findings;
};

void runHiddenContentCheck(PDFDocumentSession* session,
                           const PreflightCheckConfig& check,
                           QList<PreflightFinding>& errors,
                           QList<PreflightFinding>& warnings)
{
    if (!session || !session->getDocument())
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    PDFOptionalContentActivity printActivity(document, OCUsage::Print, nullptr);
    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();
    PDFMeshQualitySettings meshQualitySettings;

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        HiddenContentProcessor processor(page,
                                         document,
                                         session->getFontCache(),
                                         session->getCMS(),
                                         &printActivity,
                                         meshQualitySettings,
                                         session->getProcessingBudget(),
                                         check.amountPt);
        processor.processContents();

        for (const HiddenContentFinding& source : processor.findings())
        {
            if (source.type != check.id)
            {
                continue;
            }

            PreflightFinding finding;
            finding.scope = source.type == QStringLiteral("hidden-layers")
                ? QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT)
                : QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
            finding.page = int(pageIndex + 1);
            finding.type = source.type;
            finding.checkId = check.id;
            finding.bbox = source.bbox;
            finding.severity = source.heuristic && check.severity == QStringLiteral("error")
                ? QStringLiteral("info")
                : check.severity;
            finding.message = source.type == QStringLiteral("hidden-layers")
                ? PDFTranslationContext::tr("Optional-content group '%1' is not printable by default.").arg(source.detail)
                : PDFTranslationContext::tr("%1 on page %2.").arg(source.detail).arg(pageIndex + 1);
            finding.evidence.insert(QStringLiteral("confidence"), source.heuristic ? QStringLiteral("heuristic") : QStringLiteral("exact"));
            if (source.type == QStringLiteral("hidden-layers"))
            {
                finding.evidence.insert(QStringLiteral("ocg_name"), source.detail);
            }
            pushPreflightFinding(finding, finding.severity, errors, warnings);
        }
    }
}

// Scans Font resource dictionaries on the page and nested Form XObjects /
// annotation appearance streams (resource recursion with cycle guard).
void runEmbeddedFontsCheck(PDFDocumentSession* session,
                            const PreflightCheckConfig& check,
                            QList<PreflightFinding>& errors,
                            QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();
    std::set<PDFObjectReference> processedFonts;
    std::set<PDFObjectReference> processedResources;

    auto emitFontFinding = [&](int pageNumber, const QString& message)
    {
        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
        finding.page = pageNumber;
        finding.type = QStringLiteral("embedded-fonts");
        finding.severity = check.severity;
        finding.checkId = check.id;
        finding.bbox = QRectF();
        finding.message = message;
        if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
        {
            warnings.push_back(finding);
        }
        else
        {
            errors.push_back(finding);
        }
    };

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

        const PDFDictionary* fontsDict = document->getDictionaryFromObject(
            resources.getDictionary()->get("Font"));
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
                    PDFFontPointer font = PDFFont::createFont(
                        fontObj, fontsDict->getKey(i).getString(), document);
                    if (!font || font->getFontType() == FontType::Type3)
                    {
                        continue;
                    }

                    const FontDescriptor* fd = font->getFontDescriptor();
                    const QString keyName = QString::fromLatin1(fontsDict->getKey(i).getString());
                    if (!fd)
                    {
                        emitFontFinding(pageNumber,
                                        PDFTranslationContext::tr(
                                            "Font '%1' on page %2 has no font descriptor (not embedded)")
                                            .arg(keyName)
                                            .arg(pageNumber));
                        continue;
                    }

                    if (!fd->isEmbedded())
                    {
                        const QString fontName = fd->fontName.isEmpty() ? keyName : fd->fontName;
                        emitFontFinding(pageNumber,
                                        PDFTranslationContext::tr(
                                            "Font '%1' on page %2 is not embedded")
                                            .arg(fontName)
                                            .arg(pageNumber));
                    }
                }
                catch (const PDFException&)
                {
                }
            }
        }

        const PDFDictionary* xobjectDict = document->getDictionaryFromObject(
            resources.getDictionary()->get("XObject"));
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

            // Read /Subtype through the loader: PDFObject::getString() throws
            // std::bad_variant_access when the key is absent.
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

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

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

// LOW CONFIDENCE NOTE: DPI calculation uses getCurrentTransformationMatrix()
// from the PDFPageContentProcessor state, which is in PDF user space.
// This matches the existing PDFImageCollectorProcessor pattern in
// pdfimagecompressor.cpp. The identity QTransform passed to the processor
// constructor is correct because we only read the CTM from content stream
// operators, not render to a device. Page rotation is not explicitly handled
// but the existing pattern in calculateDpi works with the CTM as-is.
void runFontIntegrityCheck(PDFDocumentSession* session,
                           const PreflightCheckConfig& check,
                           QList<PreflightFinding>& errors,
                           QList<PreflightFinding>& warnings)
{
    if (!session || !session->getDocument())
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    const PDFInteger pageCount = document->getCatalog()->getPageCount();
    std::set<PDFObjectReference> processedFonts;
    std::set<PDFObjectReference> processedResources;

    auto emitFinding = [&](int pageNumber, const QString& fontName, const PDFObjectReference& reference,
                           const PDFFontIntegrityResult& result)
    {
        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
        finding.page = pageNumber;
        finding.type = QStringLiteral("font-integrity");
        finding.checkId = check.id;
        finding.objectId = QStringLiteral("%1 %2 R").arg(reference.objectNumber).arg(reference.generation);
        finding.severity = result.inspectionComplete ? check.severity : QStringLiteral("error");
        finding.message = PDFTranslationContext::tr("Font '%1' has integrity defects: %2")
            .arg(fontName, result.defects.join(QStringLiteral(", ")));
        finding.evidence.insert(QStringLiteral("font_resource"), fontName);
        finding.evidence.insert(QStringLiteral("font_subtype"), result.subtype);
        finding.evidence.insert(QStringLiteral("embedded"), true);
        finding.evidence.insert(QStringLiteral("inspection_complete"), result.inspectionComplete);
        QJsonArray defects;
        for (const QString& defect : result.defects)
        {
            defects.append(defect);
        }
        finding.evidence.insert(QStringLiteral("defects"), defects);
        pushPreflightFinding(finding, finding.severity, errors, warnings);
    };

    std::function<void(const PDFObject&, int)> scanResources;
    scanResources = [&](const PDFObject& resourcesObject, int pageNumber)
    {
        const PDFObject resources = document->getObject(resourcesObject);
        if (!resources.isDictionary())
        {
            return;
        }
        if (resourcesObject.isReference())
        {
            const PDFObjectReference reference = resourcesObject.getReference();
            if (processedResources.contains(reference))
            {
                return;
            }
            processedResources.insert(reference);
        }

        const PDFDictionary* fonts = document->getDictionaryFromObject(resources.getDictionary()->get("Font"));
        if (fonts)
        {
            for (size_t index = 0; index < fonts->getCount(); ++index)
            {
                const PDFObject fontObject = fonts->getValue(index);
                const PDFObjectReference reference = fontObject.isReference() ? fontObject.getReference() : PDFObjectReference();
                if (reference.isValid() && processedFonts.contains(reference))
                {
                    continue;
                }
                if (reference.isValid())
                {
                    processedFonts.insert(reference);
                }

                try
                {
                    PDFFontPointer font = PDFFont::createFont(fontObject, fonts->getKey(index).getString(), document);
                    if (!font || !font->getFontDescriptor() || !font->getFontDescriptor()->isEmbedded())
                    {
                        continue;
                    }
                    const PDFFontIntegrityResult result = inspectPDFFontIntegrity(*font);
                    if (!result.isClean())
                    {
                        emitFinding(pageNumber,
                                    QString::fromLatin1(fonts->getKey(index).getString()),
                                    reference,
                                    result);
                    }
                }
                catch (const PDFException& exception)
                {
                    PDFFontIntegrityResult result;
                    result.inspectionComplete = false;
                    result.defects.append(QStringLiteral("ParserException:%1").arg(QString::fromUtf8(exception.what())));
                    emitFinding(pageNumber, QString::fromLatin1(fonts->getKey(index).getString()), reference, result);
                }
            }
        }

        const PDFDictionary* xobjects = document->getDictionaryFromObject(resources.getDictionary()->get("XObject"));
        if (!xobjects)
        {
            return;
        }
        PDFDocumentDataLoaderDecorator loader(document);
        for (size_t index = 0; index < xobjects->getCount(); ++index)
        {
            const PDFObject xobject = document->getObject(xobjects->getValue(index));
            if (!xobject.isStream() || loader.readNameFromDictionary(xobject.getStream()->getDictionary(), "Subtype") != "Form")
            {
                continue;
            }
            const PDFObject formResources = xobject.getStream()->getDictionary()->get("Resources");
            if (!formResources.isNull())
            {
                scanResources(formResources, pageNumber);
            }
        }
    };

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = document->getCatalog()->getPage(pageIndex);
        if (page)
        {
            scanResources(page->getResources(), int(pageIndex + 1));
        }
    }
}

void runImageResolutionCheck(PDFDocumentSession* session,
                              const PreflightCheckConfig& check,
                              QList<PreflightFinding>& errors,
                              QList<PreflightFinding>& warnings)
{
    if (!session)
    {
        return;
    }

    if (check.minDpi <= 0)
    {
        return;
    }

    PDFDocument* document = session->getDocument();
    if (!document)
    {
        return;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();

    PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument md(document, &ocActivity);
    fontCache.setDocument(md);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFMeshQualitySettings meshQuality;

    class ImageDpiProcessor : public PDFPageContentProcessor
    {
    public:
        struct ImageDpiInfo
        {
            PDFObjectReference ref;
            qreal minDpiX = std::numeric_limits<qreal>::max();
            qreal minDpiY = std::numeric_limits<qreal>::max();
            QRectF bbox;
        };

        ImageDpiProcessor(const PDFPage* page,
                          const PDFDocument* doc,
                          const PDFFontCache* fc,
                          const PDFCMS* cms_p,
                          const PDFOptionalContentActivity* oc,
                          const PDFMeshQualitySettings& mq,
                          PDFProcessingBudget* budget,
                          std::vector<ImageDpiInfo>* results)
            : PDFPageContentProcessor(page, doc, fc, cms_p, oc, QTransform(), mq, budget)
            , m_results(results)
        {
        }

        void processFormStream(const PDFStream* stream)
        {
            if (!stream || isContentSuppressed())
            {
                return;
            }
            processForm(stream);
        }

    protected:
        bool isContentKindSuppressed(ContentKind kind) const override
        {
            switch (kind)
            {
                case ContentKind::Images:
                case ContentKind::Tiling:
                case ContentKind::Forms:
                    return false;
                default:
                    return true;
            }
        }

        bool performOriginalImagePainting(const PDFImage& image,
                                           const PDFStream* stream,
                                           PDFObjectReference reference) override
        {
            Q_UNUSED(stream);
            if (isContentSuppressed())
            {
                return true;
            }

            const QTransform ctm = getGraphicState()->getCurrentTransformationMatrix();

            const auto axisLength = [](qreal x, qreal y) -> double {
                return std::hypot(static_cast<double>(x), static_cast<double>(y)) * PDF_POINT_TO_INCH;
            };

            const double widthInches = axisLength(ctm.m11(), ctm.m12());
            const double heightInches = axisLength(ctm.m21(), ctm.m22());

            if (widthInches <= std::numeric_limits<double>::epsilon() ||
                heightInches <= std::numeric_limits<double>::epsilon())
            {
                return true;
            }

            ImageDpiInfo info;
            info.ref = reference;
            info.minDpiX = static_cast<qreal>(image.getImageData().getWidth()) / widthInches;
            info.minDpiY = static_cast<qreal>(image.getImageData().getHeight()) / heightInches;
            info.bbox = imageBoundsFromCtm(ctm,
                                           image.getImageData().getWidth(),
                                           image.getImageData().getHeight());
            m_results->push_back(info);
            return true;
        }

    private:
        std::vector<ImageDpiInfo>* m_results;
    };

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        std::vector<ImageDpiProcessor::ImageDpiInfo> images;
        ImageDpiProcessor processor(page, document, &fontCache, cms.get(), &ocActivity, meshQuality, session->getProcessingBudget(), &images);
        processor.processContents();

        processAnnotationAppearanceStreams(document, page, int(pageIndex + 1), [&](const PDFPage* /*pageRef*/, const PDFStream* formStream) {
            processor.processFormStream(formStream);
        });

        for (const auto& img : images)
        {
            const qreal dpi = qMin(img.minDpiX, img.minDpiY);
            if (dpi < static_cast<qreal>(check.minDpi))
            {
                PreflightFinding finding;
                finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_OBJECT);
                finding.page = int(pageIndex + 1);
                finding.objectId = img.ref.isValid()
                    ? QString::number(img.ref.objectNumber) : QString();
                finding.type = QStringLiteral("image-resolution");
                finding.severity = check.severity;
                finding.checkId = check.id;
                finding.bbox = img.bbox;
                finding.message = PDFTranslationContext::tr(
                    "Image resolution %1 DPI is below minimum %2 DPI on page %3")
                    .arg(qRound(dpi))
                    .arg(check.minDpi)
                    .arg(pageIndex + 1);

                if (check.severity == QStringLiteral("warning") || check.severity == QStringLiteral("info"))
                {
                    warnings.push_back(finding);
                }
                else
                {
                    errors.push_back(finding);
                }
            }
        }
    }
}

PDFXRuleResult makePDFXRuleResult(const QString& ruleId,
                                  bool mandatory,
                                  PDFXRuleState state,
                                  const QJsonObject& evidence = QJsonObject(),
                                  const QString& diagnostic = QString())
{
    PDFXRuleResult result;
    result.ruleId = ruleId;
    result.mandatory = mandatory;
    result.state = state;
    result.evidence = evidence;
    result.diagnostic = diagnostic;
    return result;
}

bool isValidPDFBox(const QRectF& box)
{
    return box.isValid() && box.width() > 0.0 && box.height() > 0.0;
}

PDFXRuleResult evaluatePDFXRule(PDFDocumentSession* session,
                                const PDFXPolicy& policy,
                                const PDFXRuleRequirement& requirement)
{
    PDFDocument* document = session ? session->getDocument() : nullptr;
    if (!document)
    {
        return makePDFXRuleResult(requirement.ruleId,
                                  requirement.mandatory,
                                  PDFXRuleState::NotInspected,
                                  QJsonObject(),
                                  PDFTranslationContext::tr("The PDF document was not available."));
    }

    const PDFCatalog* catalog = document->getCatalog();
    if (!catalog)
    {
        return makePDFXRuleResult(requirement.ruleId,
                                  requirement.mandatory,
                                  PDFXRuleState::NotInspected,
                                  QJsonObject(),
                                  PDFTranslationContext::tr("The PDF catalog could not be inspected."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.document.version"))
    {
        const QString version = QString::fromLatin1(document->getVersion());
        if (version.isEmpty())
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::NotInspected,
                                      QJsonObject(), PDFTranslationContext::tr("The PDF version could not be determined."));
        }

        const QStringList parts = version.split(QLatin1Char('.'));
        bool majorOk = false;
        bool minorOk = false;
        const int major = parts.value(0).toInt(&majorOk);
        const int minor = parts.value(1).toInt(&minorOk);
        const bool validVersion = majorOk && minorOk;
        // PDF/X-1a:2001 is based on PDF 1.3. PDF/X-4 permits PDF 1.4 and
        // later; requiring 1.6 would reject otherwise valid PDF/X-4 files.
        const QString minimumVersion = policy.flavor == PDFXFlavor::X4
            ? QStringLiteral("1.4") : QStringLiteral("1.3");
        const bool allowed = validVersion && (policy.flavor == PDFXFlavor::X4
                                                  ? (major > 1 || (major == 1 && minor >= 4))
                                                  : (major == 1 && minor == 3));
        const QJsonObject evidence{
            { QStringLiteral("document_version"), version },
            { QStringLiteral("minimum_version"), minimumVersion }
        };
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  allowed ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  evidence,
                                  allowed ? QString() : PDFTranslationContext::tr("The document version is not permitted by the selected PDF/X policy."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.document.trailer-id"))
    {
        const QByteArray firstId = document->getIdPart(0);
        const QByteArray secondId = document->getIdPart(1);
        const bool present = !firstId.isEmpty() && !secondId.isEmpty();
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  present ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  QJsonObject{
                                      { QStringLiteral("first_present"), !firstId.isEmpty() },
                                      { QStringLiteral("second_present"), !secondId.isEmpty() }
                                  },
                                  present ? QString() : PDFTranslationContext::tr("The trailer does not contain the two-part document identifier required by PDF/X."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.document.encryption"))
    {
        const PDFSecurityHandler* securityHandler = document->getStorage().getSecurityHandler();
        if (!securityHandler)
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::NotInspected,
                                      QJsonObject(), PDFTranslationContext::tr("The document security handler was not available."));
        }

        const bool encrypted = securityHandler->getMode() != EncryptionMode::None;
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  encrypted ? PDFXRuleState::Failed : PDFXRuleState::Passed,
                                  QJsonObject{
                                      { QStringLiteral("encrypted"), encrypted }
                                  },
                                  encrypted ? PDFTranslationContext::tr("Encrypted documents are not permitted by the selected PDF/X policy.") : QString());
    }

    if (requirement.ruleId == QStringLiteral("pdfx.metadata.identification"))
    {
        const PDFObject metadataObject = document->getObject(catalog->getMetadata());
        if (!metadataObject.isStream())
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::Failed,
                                      QJsonObject{{ QStringLiteral("metadata_stream"), false }},
                                      PDFTranslationContext::tr("PDF/X identification metadata is missing."));
        }

        QByteArray metadata;
        try
        {
            metadata = document->getDecodedStream(metadataObject.getStream(), session->getProcessingBudget());
        }
        catch (const PDFException& exception)
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::NotInspected,
                                      QJsonObject{{ QStringLiteral("metadata_stream"), true }}, exception.getMessage());
        }

        const QByteArray lowerMetadata = metadata.toLower();
        const bool hasPdfxIdentification = lowerMetadata.contains("pdfaid:")
            || lowerMetadata.contains("gts_pdfxversion")
            || lowerMetadata.contains("pdf/x-");
        const bool hasTargetMarker = policy.flavor == PDFXFlavor::X1a2001
            ? (lowerMetadata.contains("pdfaid:part=\"1\"") || lowerMetadata.contains("pdf/x-1a"))
            : (lowerMetadata.contains("pdfaid:part=\"4\"") || lowerMetadata.contains("pdf/x-4"));
        const bool identified = hasPdfxIdentification && hasTargetMarker;
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  identified ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  QJsonObject{
                                      { QStringLiteral("metadata_stream"), true },
                                      { QStringLiteral("has_pdfx_identification"), hasPdfxIdentification },
                                      { QStringLiteral("has_target_marker"), hasTargetMarker }
                                  },
                                  identified ? QString() : PDFTranslationContext::tr("PDF/X metadata does not identify the requested target."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.output-intent.present")
        || requirement.ruleId == QStringLiteral("pdfx.output-intent.identity")
        || requirement.ruleId == QStringLiteral("pdfx.output-intent.subtype")
        || requirement.ruleId == QStringLiteral("pdfx.output-intent.profile")
        || requirement.ruleId == QStringLiteral("pdfx.output-intent.profile-space"))
    {
        const auto& intents = catalog->getOutputIntents();
        if (intents.empty())
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::Failed,
                                      QJsonObject{{ QStringLiteral("count"), 0 }},
                                      PDFTranslationContext::tr("No PDF/X output intent is defined."));
        }

        int subtypeFailures = 0;
        int identityFailures = 0;
        int profileFailures = 0;
        int profileSpaceFailures = 0;
        int profileSpaceUninspected = 0;
        QJsonArray spaces;
        for (const PDFOutputIntent& intent : intents)
        {
            if (intent.getOutputConditionIdentifier().trimmed().isEmpty())
            {
                ++identityFailures;
            }

            if (intent.getSubtype() != QByteArrayLiteral("GTS_PDFX"))
            {
                ++subtypeFailures;
            }

            const PDFObject profileObject = document->getObject(intent.getOutputProfile());
            if (!profileObject.isStream())
            {
                ++profileFailures;
                ++profileSpaceUninspected;
                continue;
            }

            QByteArray profileBytes;
            try
            {
                profileBytes = document->getDecodedStream(profileObject.getStream(), session->getProcessingBudget());
            }
            catch (const PDFException&)
            {
                ++profileFailures;
                ++profileSpaceUninspected;
                continue;
            }

            IccProfileGuard profile(profileBytes);
            if (!profile.isValid())
            {
                ++profileFailures;
                ++profileSpaceUninspected;
                continue;
            }

            const QString colorSpace = classifyIccColorSpace(profile.getColorSpace());
            spaces.append(colorSpace);
            const QByteArray declared = intent.getOutputProfileInfo().getSignature();
            if (colorSpace.isEmpty() || declared.isEmpty()
                || QString::fromLatin1(declared).compare(colorSpace, Qt::CaseInsensitive) != 0)
            {
                ++profileSpaceFailures;
            }
        }

        if (requirement.ruleId == QStringLiteral("pdfx.output-intent.subtype"))
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      subtypeFailures == 0 ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                      QJsonObject{
                                          { QStringLiteral("count"), int(intents.size()) },
                                          { QStringLiteral("invalid_subtypes"), subtypeFailures }
                                      },
                                      subtypeFailures == 0 ? QString() : PDFTranslationContext::tr("Every output intent must use the GTS_PDFX subtype."));
        }

        if (requirement.ruleId == QStringLiteral("pdfx.output-intent.identity"))
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      identityFailures == 0 ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                      QJsonObject{
                                          { QStringLiteral("count"), int(intents.size()) },
                                          { QStringLiteral("missing_identifiers"), identityFailures }
                                      },
                                      identityFailures == 0 ? QString() : PDFTranslationContext::tr("Every output intent must identify its intended printing condition."));
        }

        if (requirement.ruleId == QStringLiteral("pdfx.output-intent.profile"))
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      profileFailures == 0 ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                      QJsonObject{
                                          { QStringLiteral("count"), int(intents.size()) },
                                          { QStringLiteral("invalid_profiles"), profileFailures }
                                      },
                                      profileFailures == 0 ? QString() : PDFTranslationContext::tr("Every output intent must contain a valid, decodable ICC profile."));
        }

        if (requirement.ruleId == QStringLiteral("pdfx.output-intent.profile-space"))
        {
            if (profileSpaceUninspected > 0)
            {
                return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                          PDFXRuleState::NotInspected,
                                          QJsonObject{
                                              { QStringLiteral("uninspected_profiles"), profileSpaceUninspected },
                                              { QStringLiteral("mismatches"), profileSpaceFailures }
                                          },
                                          PDFTranslationContext::tr("The output-intent profile color space could not be verified for every intent."));
            }
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      profileSpaceFailures == 0 ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                      QJsonObject{
                                          { QStringLiteral("color_spaces"), spaces },
                                          { QStringLiteral("mismatches"), profileSpaceFailures }
                                      },
                                      profileSpaceFailures == 0 ? QString() : PDFTranslationContext::tr("An output intent declares a color space that does not match its embedded ICC profile."));
        }

        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::Passed,
                                  QJsonObject{{ QStringLiteral("count"), int(intents.size()) }});
    }

    if (requirement.ruleId == QStringLiteral("pdfx.page.trim-box")
        || requirement.ruleId == QStringLiteral("pdfx.page.bleed-box"))
    {
        QJsonArray missingPages;
        const bool trim = requirement.ruleId.endsWith(QStringLiteral("trim-box"));
        for (PDFInteger pageIndex = 0; pageIndex < catalog->getPageCount(); ++pageIndex)
        {
            const PDFPage* page = catalog->getPage(pageIndex);
            if (!page || !isValidPDFBox(trim ? page->getTrimBox() : page->getBleedBox()))
            {
                missingPages.append(int(pageIndex + 1));
            }
        }

        const bool passed = missingPages.isEmpty();
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  passed ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  QJsonObject{
                                      { QStringLiteral("page_count"), int(catalog->getPageCount()) },
                                      { QStringLiteral("missing_pages"), missingPages }
                                  },
                                  passed ? QString() : PDFTranslationContext::tr("One or more pages do not have the required inherited page box."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.font.embedded"))
    {
        PreflightCheckConfig fontCheck;
        fontCheck.id = QStringLiteral("embedded-fonts");
        fontCheck.severity = QStringLiteral("error");
        QList<PreflightFinding> errors;
        QList<PreflightFinding> warnings;
        runEmbeddedFontsCheck(session, fontCheck, errors, warnings);
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  errors.isEmpty() ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  QJsonObject{
                                      { QStringLiteral("failed_fonts"), int(errors.size()) },
                                      { QStringLiteral("warnings"), int(warnings.size()) }
                                  },
                                  errors.isEmpty() ? QString() : PDFTranslationContext::tr("One or more fonts are not embedded."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.color.device-rgb"))
    {
        if (policy.flavor != PDFXFlavor::X1a2001)
        {
            return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::NotApplicable,
                                      QJsonObject{{ QStringLiteral("target_allows_device_rgb"), true }});
        }

        PreflightCheckConfig colorCheck;
        colorCheck.id = QStringLiteral("color-mode");
        colorCheck.severity = QStringLiteral("error");
        colorCheck.allowedColorModes = { QStringLiteral("CMYK"), QStringLiteral("Grayscale") };
        QList<PreflightFinding> errors;
        QList<PreflightFinding> warnings;
        runColorModeCheck(session, colorCheck, errors, warnings);
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  errors.isEmpty() ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  QJsonObject{
                                      { QStringLiteral("disallowed_pages"), int(errors.size()) },
                                      { QStringLiteral("allowed"), QJsonArray{ QStringLiteral("CMYK"), QStringLiteral("Grayscale") } }
                                  },
                                  errors.isEmpty() ? QString() : PDFTranslationContext::tr("DeviceRGB content is not permitted by PDF/X-1a:2001."));
    }

    if (requirement.ruleId == QStringLiteral("pdfx.transparency.allowed"))
    {
        int transparencyObjects = 0;
        const PDFObjectStorage::PDFObjects& objects = document->getStorage().getObjects();
        PDFDocumentDataLoaderDecorator loader(document);
        for (const auto& entry : objects)
        {
            const PDFObject& object = entry.object;
            const PDFDictionary* dictionary = document->getDictionaryFromObject(object);
            if (!dictionary)
            {
                continue;
            }

            bool transparent = loader.readNameFromDictionary(dictionary, "S") == QByteArrayLiteral("Transparency")
                || dictionary->hasKey("SMask")
                || dictionary->hasKey("ca")
                || dictionary->hasKey("CA");
            if (dictionary->hasKey("BM"))
            {
                const QByteArray blendMode = loader.readNameFromDictionary(dictionary, "BM");
                transparent = transparent || (!blendMode.isEmpty()
                                               && blendMode != QByteArrayLiteral("Normal")
                                               && blendMode != QByteArrayLiteral("Compatible"));
            }
            if (transparent)
            {
                ++transparencyObjects;
            }
        }

        const bool forbidden = policy.flavor == PDFXFlavor::X1a2001 && transparencyObjects > 0;
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  forbidden ? PDFXRuleState::Failed : PDFXRuleState::Passed,
                                  QJsonObject{
                                      { QStringLiteral("transparency_objects"), transparencyObjects },
                                      { QStringLiteral("target_allows_live_transparency"), !forbidden }
                                  },
                                  forbidden ? PDFTranslationContext::tr("Live transparency is not permitted by PDF/X-1a:2001.") : QString());
    }

    if (requirement.ruleId == QStringLiteral("pdfx.overprint.inspectable"))
    {
        int overprintObjects = 0;
        for (const auto& entry : document->getStorage().getObjects())
        {
            const PDFDictionary* dictionary = document->getDictionaryFromObject(entry.object);
            if (dictionary && (dictionary->hasKey("OP") || dictionary->hasKey("op") || dictionary->hasKey("OPM")))
            {
                ++overprintObjects;
            }
        }
        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory, PDFXRuleState::Passed,
                                  QJsonObject{
                                      { QStringLiteral("overprint_objects"), overprintObjects },
                                      { QStringLiteral("renderer"), QStringLiteral("Output Preview separation/overprint path") },
                                      { QStringLiteral("inspection"), QStringLiteral("structural flags inspected; rendering remains authoritative in Output Preview") }
                                  });
    }

    if (requirement.ruleId == QStringLiteral("pdfx.annotation.forbidden-action"))
    {
        int actionCount = catalog->getOpenAction() ? 1 : 0;
        for (const auto& action : catalog->getDocumentActions())
        {
            actionCount += action ? 1 : 0;
        }
        actionCount += int(catalog->getNamedJavaScriptActions().size());

        QJsonArray actionPages;
        for (PDFInteger pageIndex = 0; pageIndex < catalog->getPageCount(); ++pageIndex)
        {
            const PDFPage* page = catalog->getPage(pageIndex);
            if (!page)
            {
                continue;
            }
            for (const PDFObjectReference& annotationReference : page->getAnnotations())
            {
                const PDFObject annotation = document->getObjectByReference(annotationReference);
                const PDFDictionary* annotationDictionary = document->getDictionaryFromObject(annotation);
                if (annotationDictionary && (annotationDictionary->hasKey("A") || annotationDictionary->hasKey("AA")))
                {
                    ++actionCount;
                    actionPages.append(int(pageIndex + 1));
                }
            }
        }

        return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                  actionCount == 0 ? PDFXRuleState::Passed : PDFXRuleState::Failed,
                                  QJsonObject{
                                      { QStringLiteral("action_count"), actionCount },
                                      { QStringLiteral("annotation_pages"), actionPages }
                                  },
                                  actionCount == 0 ? QString() : PDFTranslationContext::tr("Active document or annotation actions are not permitted by the selected PDF/X policy."));
    }

    return makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                              PDFXRuleState::NotInspected,
                              QJsonObject(),
                              PDFTranslationContext::tr("This mandatory PDF/X rule is not implemented by the current policy capability set."));
}

PDFXConformanceResult evaluatePDFXPolicy(PDFDocumentSession* session, const PDFXPolicy& policy)
{
    PDFXConformanceResult result;
    result.requestedFlavor = policy.flavor;
    result.policyVersion = policy.policyVersion;

    for (const PDFXRuleRequirement& requirement : policy.rules)
    {
        PDFXRuleResult rule;
        try
        {
            rule = evaluatePDFXRule(session, policy, requirement);
        }
        catch (const PDFException& exception)
        {
            rule = makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      PDFXRuleState::NotInspected, QJsonObject(), exception.getMessage());
        }
        catch (const std::exception& exception)
        {
            rule = makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      PDFXRuleState::NotInspected, QJsonObject(), QString::fromUtf8(exception.what()));
        }
        catch (...)
        {
            rule = makePDFXRuleResult(requirement.ruleId, requirement.mandatory,
                                      PDFXRuleState::NotInspected, QJsonObject(), PDFTranslationContext::tr("Unknown error."));
        }

        result.rules.push_back(rule);
    }

    result.status = reducePDFXStatus(result.rules, &result.failedRuleIds, &result.incompleteRuleIds);

    return result;
}

void appendPDFXFindings(const PDFXConformanceResult& result,
                        QList<PreflightFinding>& errors,
                        QList<PreflightFinding>& warnings)
{
    for (const PDFXRuleResult& rule : result.rules)
    {
        const bool mandatoryNotApplicable = rule.mandatory && rule.state == PDFXRuleState::NotApplicable;
        if (rule.state != PDFXRuleState::Failed
            && rule.state != PDFXRuleState::NotInspected
            && !mandatoryNotApplicable)
        {
            continue;
        }

        QJsonObject evidence{
            { QStringLiteral("rule_id"), rule.ruleId },
            { QStringLiteral("mandatory"), rule.mandatory },
            { QStringLiteral("state"), pdfxRuleStateToString(rule.state) }
        };
        if (!rule.evidence.isEmpty())
        {
            evidence.insert(QStringLiteral("rule"), rule.evidence);
        }

        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.type = rule.state == PDFXRuleState::Failed
            ? QStringLiteral("pdfx-conformance") : QStringLiteral("pdfx-incomplete");
        finding.severity = rule.state == PDFXRuleState::Failed
            ? QStringLiteral("error") : QStringLiteral("warning");
        // Keep the stable rule ID in the canonical finding field so callers do
        // not need to parse prose or inspect nested evidence to route it.
        finding.checkId = rule.ruleId;
        finding.evidence = evidence;
        finding.message = rule.state == PDFXRuleState::Failed
            ? PDFTranslationContext::tr("PDF/X rule '%1' failed: %2").arg(rule.ruleId, rule.diagnostic)
            : PDFTranslationContext::tr("PDF/X rule '%1' could not be inspected: %2").arg(rule.ruleId, rule.diagnostic);

        if (rule.state == PDFXRuleState::Failed)
        {
            errors.push_back(finding);
        }
        else
        {
            warnings.push_back(finding);
        }
    }
}

} // namespace

QJsonObject PreflightResult::toJson(const QString& pdfPath) const
{
    QJsonArray errorsArray;
    for (const PreflightFinding& finding : errors)
    {
        errorsArray.append(findingToJson(finding));
    }

    QJsonArray warningsArray;
    for (const PreflightFinding& finding : warnings)
    {
        warningsArray.append(findingToJson(finding));
    }

    QJsonArray fixupsArray;
    for (const PreflightFixupConfig& fixup : fixupsAvailable)
    {
        QJsonObject fixupObject;
        fixupObject.insert(QStringLiteral("id"), fixup.id);
        fixupObject.insert(QStringLiteral("safe"), false);
        fixupObject.insert(QStringLiteral("description"), fixup.description.isEmpty() ? defaultFixupDescription(fixup.id) : fixup.description);

        QJsonObject params = fixup.params;
        if (fixup.amountPt > 0.0 && !params.contains(QStringLiteral("amount_pt")))
        {
            params.insert(QStringLiteral("amount_pt"), fixup.amountPt);
        }
        if (!params.isEmpty())
        {
            fixupObject.insert(QStringLiteral("params"), params);
        }

        fixupsArray.append(fixupObject);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), PREFLIGHT_REPORT_SCHEMA_VERSION);
    root.insert(QStringLiteral("inspection_complete"), inspectionComplete);
    const PreflightVerdict verdict = reducePreflightVerdict(*this);
    root.insert(QStringLiteral("pass"), verdict.isPass());
    root.insert(QStringLiteral("verdict"), verdict.toJson());
    if (!errorCode.isEmpty() || !errorMessage.isEmpty())
    {
        root.insert(QStringLiteral("error"), QJsonObject{
            { QStringLiteral("code"), errorCode },
            { QStringLiteral("message"), errorMessage }
        });
    }
    root.insert(QStringLiteral("profile"), profileName);
    root.insert(QStringLiteral("engine_version"), QCoreApplication::applicationVersion());
    if (!pdfPath.isEmpty())
    {
        root.insert(QStringLiteral("pdf"), pdfPath);
    }
    if (!documentRevisionDigest.isEmpty())
    {
        root.insert(QStringLiteral("document_revision_digest"), documentRevisionDigest);
    }
    if (!effectiveProfileDigest.isEmpty())
    {
        root.insert(QStringLiteral("effective_profile_digest"), effectiveProfileDigest);
    }
    root.insert(QStringLiteral("errors"), errorsArray);
    root.insert(QStringLiteral("warnings"), warningsArray);
    root.insert(QStringLiteral("fixups_available"), fixupsArray);

    QJsonArray decisionsArray;
    for (const PreflightDecision& decision : decisions)
    {
        decisionsArray.append(decision.toJson(documentRevisionDigest, effectiveProfileDigest));
    }
    root.insert(QStringLiteral("decisions"), decisionsArray);

    if (!profileResolution.isEmpty())
    {
        root.insert(QStringLiteral("profile_resolution"), profileResolution);
    }

    QJsonArray checksArray;
    for (const PreflightCheckStatus& status : checkStatuses)
    {
        QJsonObject checkObject;
        checkObject.insert(QStringLiteral("id"), status.id);
        checkObject.insert(QStringLiteral("status"), status.status);
        if (!status.reason.isEmpty())
        {
            checkObject.insert(QStringLiteral("reason"), status.reason);
        }
        if (!status.budgetKind.isEmpty())
        {
            QJsonObject budgetObject;
            budgetObject.insert(QStringLiteral("kind"), status.budgetKind);
            budgetObject.insert(QStringLiteral("limit"), status.budgetLimit);
            budgetObject.insert(QStringLiteral("attempted"), status.budgetAttempted);
            if (!status.budgetContext.isEmpty())
            {
                budgetObject.insert(QStringLiteral("context"), status.budgetContext);
            }
            checkObject.insert(QStringLiteral("budget"), budgetObject);
        }
        checksArray.append(checkObject);
    }
    root.insert(QStringLiteral("checks"), checksArray);

    if (pdfx.has_value())
    {
        root.insert(QStringLiteral("pdfx"), pdfx->toJson());
    }

    return root;
}

PreflightEngine::PreflightEngine(PDFDocumentSession* session) :
    m_session(session)
{
    registerBuiltInChecks();
}

void PreflightEngine::registerCheck(const QString& id, CheckRunner runner)
{
    m_checks[id] = std::move(runner);
}

bool PreflightEngine::hasCheck(const QString& id) const
{
    return m_checks.count(id) > 0;
}

PreflightResult PreflightEngine::run(const QJsonObject& profile)
{
    PreflightProfileData data;
    QString errorMessage;
    if (!parseProfile(profile, data, errorMessage))
    {
        // Profile parsing errors are retained in the normalized report so the
        // canonical reducer can classify them as an operator-visible error.
        PreflightResult result;
        result.errorCode = QStringLiteral("profile-invalid");
        result.errorMessage = errorMessage;
        result.profileName = profile.value(QStringLiteral("name")).toString();

        PreflightFinding finding;
        finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.type = QStringLiteral("profile");
        finding.severity = QStringLiteral("error");
        finding.message = errorMessage;
        finding.bbox = QRectF();
        result.errors.push_back(finding);

        return result;
    }

    return run(data);
}

PreflightResult PreflightEngine::run(const PreflightProfileData& profile)
{
    PreflightResult result;
    result.profileName = profile.name;
    result.inspectionComplete = true;
    if (m_session)
    {
        m_session->resetProcessingBudget();
    }

    for (const PreflightCheckConfig& check : profile.checks)
    {
        PreflightCheckStatus status;
        status.id = check.id;

        if (!check.enabled)
        {
            status.status = QStringLiteral("skipped");
            status.reason = QStringLiteral("disabled");
            result.checkStatuses.push_back(status);
            continue;
        }

        auto it = m_checks.find(check.id);
        if (it == m_checks.end())
        {
            status.status = QStringLiteral("unsupported");
            status.reason = QStringLiteral("unknown check id");
            result.checkStatuses.push_back(status);
            result.inspectionComplete = false;

            PreflightFinding finding;
            finding.scope = QString::fromLatin1(PREFLIGHT_FINDING_SCOPE_DOCUMENT);
            finding.type = QStringLiteral("profile");
            finding.severity = QStringLiteral("error");
            finding.checkId = check.id;
            finding.message = PDFTranslationContext::tr("Unknown preflight check '%1'.").arg(check.id);
            result.errors.push_back(finding);
            if (result.errorCode.isEmpty())
            {
                result.errorCode = QStringLiteral("unsupported-check");
                result.errorMessage = finding.message;
            }
            continue;
        }

        const int errorsBefore = result.errors.size();
        const int warningsBefore = result.warnings.size();

        // A malformed document must not be able to take the whole run down: a
        // check that throws is contained, reported, and the remaining checks
        // still run.
        try
        {
            it->second(m_session, check, result.errors, result.warnings);
        }
        catch (const PDFBudgetExceededException& exception)
        {
            recordBudgetFailure(result, status, check, exception);
            continue;
        }
        catch (const PDFException& exception)
        {
            recordCheckFailure(result, status, check, exception.getMessage());
            continue;
        }
        catch (const std::exception& exception)
        {
            recordCheckFailure(result, status, check, QString::fromUtf8(exception.what()));
            continue;
        }
        catch (...)
        {
            recordCheckFailure(result, status, check, PDFTranslationContext::tr("Unknown error."));
            continue;
        }

        const auto isCheckIncomplete = [](const PreflightFinding& finding)
        {
            return finding.type == QStringLiteral("check-incomplete");
        };
        const bool checkIncomplete = std::any_of(result.errors.cbegin() + errorsBefore,
                                                  result.errors.cend(),
                                                  isCheckIncomplete)
            || std::any_of(result.warnings.cbegin() + warningsBefore,
                           result.warnings.cend(),
                           isCheckIncomplete);
        const bool checkFailed = result.errors.size() > errorsBefore;
        const bool checkWarned = std::any_of(result.warnings.cbegin() + warningsBefore,
                                              result.warnings.cend(),
                                              [](const PreflightFinding& finding)
        {
            return finding.severity == QStringLiteral("warning");
        });
        if (checkIncomplete)
        {
            status.status = QStringLiteral("skipped");
            status.reason = QStringLiteral("inspection incomplete");
            const auto recordIncompleteReason = [&status](const PreflightFinding& finding)
            {
                if (finding.type == QStringLiteral("check-incomplete"))
                {
                    const QString reason = finding.evidence.value(QStringLiteral("reason")).toString();
                    if (!reason.isEmpty())
                    {
                        status.reason = reason;
                    }
                }
            };
            std::for_each(result.errors.cbegin() + errorsBefore, result.errors.cend(), recordIncompleteReason);
            std::for_each(result.warnings.cbegin() + warningsBefore, result.warnings.cend(), recordIncompleteReason);
            result.inspectionComplete = false;
        }
        else if (checkFailed)
        {
            status.status = QStringLiteral("failed");
        }
        else if (checkWarned)
        {
            status.status = QStringLiteral("warning");
        }
        else
        {
            status.status = QStringLiteral("ok");
        }
        result.checkStatuses.push_back(status);
    }

    if (profile.pdfx.has_value())
    {
        const PDFXConformanceResult pdfxResult = evaluatePDFXPolicy(m_session, profile.pdfx.value());
        result.pdfx = pdfxResult;
        appendPDFXFindings(pdfxResult, result.errors, result.warnings);

        PreflightCheckStatus pdfxStatus;
        pdfxStatus.id = QStringLiteral("pdfx");
        if (pdfxResult.status == PDFXConformanceStatus::NonConformant)
        {
            pdfxStatus.status = QStringLiteral("failed");
            pdfxStatus.reason = PDFTranslationContext::tr("Mandatory PDF/X rules failed.");
        }
        else if (pdfxResult.status == PDFXConformanceStatus::Incomplete)
        {
            pdfxStatus.status = QStringLiteral("unsupported");
            pdfxStatus.reason = PDFTranslationContext::tr("Mandatory PDF/X evidence was not available.");
            result.inspectionComplete = false;
        }
        else
        {
            pdfxStatus.status = QStringLiteral("ok");
        }
        if (!pdfxResult.incompleteRuleIds.isEmpty())
        {
            // A definite failure wins the normalized PDF/X status, but any
            // missing mandatory evidence still makes the overall inspection
            // incomplete and must remain visible to callers.
            result.inspectionComplete = false;
        }
        result.checkStatuses.push_back(pdfxStatus);
    }

    result.fixupsAvailable = profile.fixups;

    qreal addBleedAmountPt = 0.0;
    for (const PreflightCheckConfig& check : profile.checks)
    {
        if (check.enabled && (check.id == QStringLiteral("bleed") || check.id == QStringLiteral("content-bleed")) && check.amountPt > 0.0)
        {
            addBleedAmountPt = check.amountPt;
            break;
        }
    }

    const bool needsAddBleed = hasBleedGapFinding(result.errors) || hasBleedGapFinding(result.warnings);
    adjustFixupsAvailable(m_session,
                          result.fixupsAvailable,
                          needsAddBleed,
                          addBleedAmountPt,
                          result.errors,
                          result.warnings);

    result.pass = reducePreflightVerdict(result, &profile).isPass();

    return result;
}

PDFDocumentSession* PreflightEngine::getSession() const
{
    return m_session;
}

bool PreflightEngine::loadProfile(const QString& profilePath, QJsonObject& profile, QString& errorMessage)
{
    QFile profileFile(profilePath);
    if (!profileFile.open(QIODevice::ReadOnly))
    {
        errorMessage = PDFTranslationContext::tr("Cannot open profile '%1'.").arg(profilePath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(profileFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = PDFTranslationContext::tr("Invalid profile JSON in '%1': %2").arg(profilePath, parseError.errorString());
        return false;
    }

    profile = document.object();
    return true;
}

bool PreflightEngine::parseProfile(const QJsonObject& profileObject, PreflightProfileData& profile, QString& errorMessage)
{
    errorMessage.clear();
    profile = PreflightProfileData();

    profile.name = profileObject.value(QStringLiteral("name")).toString();
    if (profile.name.isEmpty())
    {
        errorMessage = PDFTranslationContext::tr("Profile is missing required field 'name'.");
        return false;
    }

    const QJsonArray checks = profileObject.value(QStringLiteral("checks")).toArray();
    if (checks.isEmpty())
    {
        errorMessage = PDFTranslationContext::tr("Profile must define at least one check.");
        return false;
    }

    if (profileObject.contains(QStringLiteral("pdfx")))
    {
        const QJsonValue pdfxValue = profileObject.value(QStringLiteral("pdfx"));
        if (!pdfxValue.isObject())
        {
            errorMessage = PDFTranslationContext::tr("Profile field 'pdfx' must be an object.");
            return false;
        }

        const QJsonObject pdfxObject = pdfxValue.toObject();
        for (const QString& key : pdfxObject.keys())
        {
            if (key != QStringLiteral("target") && key != QStringLiteral("policyVersion"))
            {
                errorMessage = PDFTranslationContext::tr("Profile field 'pdfx.%1' is not supported.").arg(key);
                return false;
            }
        }
        const QJsonValue targetValue = pdfxObject.value(QStringLiteral("target"));
        if (!targetValue.isString() || targetValue.toString().isEmpty())
        {
            errorMessage = PDFTranslationContext::tr("Profile field 'pdfx.target' must be a non-empty string.");
            return false;
        }

        PDFXPolicy policy;
        if (!pdfxPolicyForTarget(targetValue.toString(), policy, errorMessage))
        {
            return false;
        }

        if (pdfxObject.contains(QStringLiteral("policyVersion")))
        {
            const QJsonValue versionValue = pdfxObject.value(QStringLiteral("policyVersion"));
            const bool validVersion = (versionValue.isString() && versionValue.toString() == policy.policyVersion)
                || (versionValue.isDouble() && qFuzzyCompare(versionValue.toDouble() + 1.0, policy.policyVersion.toDouble() + 1.0));
            if (!validVersion)
            {
                errorMessage = PDFTranslationContext::tr(
                    "Profile field 'pdfx.policyVersion' must match supported policy revision %1.").arg(policy.policyVersion);
                return false;
            }
        }

        profile.pdfx = policy;
    }

    for (const QJsonValue& checkValue : checks)
    {
        const QJsonObject checkObject = checkValue.toObject();
        PreflightCheckConfig check;
        check.id = checkObject.value(QStringLiteral("id")).toString();
        if (check.id.isEmpty())
        {
            errorMessage = PDFTranslationContext::tr("Profile contains a check without 'id'.");
            return false;
        }

        check.severity = checkObject.value(QStringLiteral("severity")).toString(check.severity);
        if (check.severity != QStringLiteral("error") &&
            check.severity != QStringLiteral("warning") &&
            check.severity != QStringLiteral("info"))
        {
            errorMessage = PDFTranslationContext::tr("Check '%1' has invalid severity '%2' (must be 'error', 'warning', or 'info').").arg(check.id, check.severity);
            return false;
        }
        check.enabled = checkObject.value(QStringLiteral("enabled")).toBool(true);
        check.amountPt = checkObject.value(QStringLiteral("amount_pt")).toDouble(check.amountPt);
        check.required = checkObject.value(QStringLiteral("required")).toBool(check.required);
        check.expectedWidthPt = checkObject.value(QStringLiteral("expected_width_pt")).toDouble(check.expectedWidthPt);
        check.expectedHeightPt = checkObject.value(QStringLiteral("expected_height_pt")).toDouble(check.expectedHeightPt);
        check.tolerancePt = checkObject.value(QStringLiteral("tolerance_pt")).toDouble(check.tolerancePt);
        check.hasExpectedSize = check.expectedWidthPt > 0.0 && check.expectedHeightPt > 0.0;

        check.rasterConfirm = checkObject.value(QStringLiteral("raster_confirm")).toBool(false);
        check.probeDpi = checkObject.value(QStringLiteral("probe_dpi")).toInt(150);
        if (check.rasterConfirm && check.probeDpi <= 0)
        {
            errorMessage = PDFTranslationContext::tr("Check '%1' requires positive probe_dpi when raster_confirm is enabled.").arg(check.id);
            return false;
        }
        check.probeThreshold = checkObject.value(QStringLiteral("probe_threshold")).toInt(16);
        check.rasterWhiteThreshold = checkObject.value(QStringLiteral("raster_white_threshold")).toDouble(0.9975);

        const QJsonValue maxInkValue = checkObject.value(QStringLiteral("max_ink_pct"));
        const QJsonValue minRegionAreaValue = checkObject.value(QStringLiteral("min_region_area_pct"));
        const QJsonValue maxRegionsValue = checkObject.value(QStringLiteral("max_regions_per_page"));
        const QJsonValue maxRasterPixelsValue = checkObject.value(QStringLiteral("max_raster_pixels"));
        check.maxInkPct = maxInkValue.toDouble(0.0);
        check.minRegionAreaPct = minRegionAreaValue.toDouble(0.05);
        check.maxRegionsPerPage = maxRegionsValue.toInt(20);
        check.maxRasterPixels = 250LL * 1000 * 1000;
        if (check.id == QStringLiteral("ink-coverage")
            && (!maxInkValue.isDouble()
                || !std::isfinite(check.maxInkPct)
                || check.maxInkPct <= 0.0))
        {
            errorMessage = PDFTranslationContext::tr("Check '%1' requires positive max_ink_pct.").arg(check.id);
            return false;
        }

        if (check.id == QStringLiteral("ink-coverage"))
        {
            if (checkObject.contains(QStringLiteral("probe_dpi"))
                && (!checkObject.value(QStringLiteral("probe_dpi")).isDouble()
                    || check.probeDpi <= 0
                    || std::floor(checkObject.value(QStringLiteral("probe_dpi")).toDouble())
                        != checkObject.value(QStringLiteral("probe_dpi")).toDouble()))
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires integral positive probe_dpi.").arg(check.id);
                return false;
            }
            if (checkObject.contains(QStringLiteral("min_region_area_pct"))
                && (!minRegionAreaValue.isDouble()
                    || !std::isfinite(check.minRegionAreaPct)
                    || check.minRegionAreaPct < 0.0
                    || check.minRegionAreaPct > 100.0))
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires min_region_area_pct between 0 and 100.").arg(check.id);
                return false;
            }
            if (checkObject.contains(QStringLiteral("max_regions_per_page"))
                && (!maxRegionsValue.isDouble()
                    || std::floor(maxRegionsValue.toDouble()) != maxRegionsValue.toDouble()
                    || check.maxRegionsPerPage < 0))
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires non-negative integral max_regions_per_page.").arg(check.id);
                return false;
            }
            if (checkObject.contains(QStringLiteral("max_raster_pixels")))
            {
                const double maxRasterPixels = maxRasterPixelsValue.toDouble(0.0);
                if (!maxRasterPixelsValue.isDouble()
                    || !std::isfinite(maxRasterPixels)
                    || std::floor(maxRasterPixels) != maxRasterPixels
                    || maxRasterPixels <= 0.0
                    || maxRasterPixels >= static_cast<double>(std::numeric_limits<qint64>::max()))
                {
                    errorMessage = PDFTranslationContext::tr("Check '%1' requires positive integral max_raster_pixels.").arg(check.id);
                    return false;
                }
                check.maxRasterPixels = static_cast<qint64>(maxRasterPixels);
            }

            const QJsonValue analysisBoxValue = checkObject.value(QStringLiteral("analysis_box"));
            if (analysisBoxValue.isUndefined())
            {
                check.inkCoverageAnalysisBox = QStringLiteral("bleed");
            }
            else if (!analysisBoxValue.isString())
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires string analysis_box.").arg(check.id);
                return false;
            }
            else
            {
                check.inkCoverageAnalysisBox = analysisBoxValue.toString();
            }

            if (check.inkCoverageAnalysisBox != QStringLiteral("bleed")
                && check.inkCoverageAnalysisBox != QStringLiteral("trim")
                && check.inkCoverageAnalysisBox != QStringLiteral("crop")
                && check.inkCoverageAnalysisBox != QStringLiteral("media"))
            {
                errorMessage = PDFTranslationContext::tr(
                    "Check '%1' has invalid analysis_box '%2' (must be 'bleed', 'trim', 'crop', or 'media').")
                    .arg(check.id, check.inkCoverageAnalysisBox);
                return false;
            }
        }

        if (check.id == QStringLiteral("thin-strokes"))
        {
            check.minEffectiveStrokeWidthPt = checkObject.value(QStringLiteral("min_effective_width_pt")).toDouble(check.minEffectiveStrokeWidthPt);
            check.zeroWidthEpsilonPt = checkObject.value(QStringLiteral("zero_width_epsilon_pt")).toDouble(check.zeroWidthEpsilonPt);
            check.hairlineSeverity = checkObject.value(QStringLiteral("hairline_severity")).toString(check.severity);
            check.thinStrokeSeverity = checkObject.value(QStringLiteral("thin_stroke_severity")).toString(check.severity);

            const auto validSeverity = [](const QString& severity)
            {
                return severity == QStringLiteral("error")
                    || severity == QStringLiteral("warning")
                    || severity == QStringLiteral("info");
            };
            if (check.minEffectiveStrokeWidthPt <= 0.0)
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires positive min_effective_width_pt.").arg(check.id);
                return false;
            }
            if (check.zeroWidthEpsilonPt < 0.0)
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires non-negative zero_width_epsilon_pt.").arg(check.id);
                return false;
            }
            if (!validSeverity(check.hairlineSeverity) || !validSeverity(check.thinStrokeSeverity))
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' has invalid hairline or thin-stroke severity.").arg(check.id);
                return false;
            }
        }

        check.minDpi = checkObject.value(QStringLiteral("min_dpi")).toInt(0);
        if (check.id == QStringLiteral("image-resolution") && check.minDpi <= 0)
        {
            errorMessage = PDFTranslationContext::tr("Check '%1' requires positive min_dpi.").arg(check.id);
            return false;
        }

        const QJsonArray allowedModes = checkObject.value(QStringLiteral("allowed")).toArray();
        for (const QJsonValue& val : allowedModes)
        {
            check.allowedColorModes.append(val.toString());
        }
        if (check.id == QStringLiteral("color-mode") && check.allowedColorModes.isEmpty())
        {
            errorMessage = PDFTranslationContext::tr("Check '%1' requires non-empty allowed color modes.").arg(check.id);
            return false;
        }

        if (check.id == QStringLiteral("processing-steps") || check.id == QStringLiteral("dieline"))
        {
            const QJsonArray requiredTypes = checkObject.value(QStringLiteral("required_types")).toArray();
            for (const QJsonValue& value : requiredTypes)
            {
                if (!value.isString() || pdfProcessingStepTypeFromString(value.toString()) == PDFProcessingStepType::Unknown)
                {
                    errorMessage = PDFTranslationContext::tr("Check '%1' contains an unknown processing step type.").arg(check.id);
                    return false;
                }
                check.requiredProcessingStepTypes.append(value.toString());
            }
        }

        if (check.id == QStringLiteral("output-intent"))
        {
            for (const QString& mode : check.allowedColorModes)
            {
                if (mode != QStringLiteral("CMYK") &&
                    mode != QStringLiteral("RGB") &&
                    mode != QStringLiteral("Grayscale"))
                {
                    errorMessage = PDFTranslationContext::tr(
                        "Check '%1' has unknown allowed color space '%2' (expected 'CMYK', 'RGB', or 'Grayscale').")
                        .arg(check.id, mode);
                    return false;
                }
            }
        }

        if (check.id == QStringLiteral("color-inventory"))
        {
            check.colorProbeDpi = checkObject.value(QStringLiteral("probe_dpi")).toInt(check.colorProbeDpi);
            const qreal richBlackPercent = checkObject.value(QStringLiteral("rich_black_k_percent")).toDouble(10.0);
            if (check.colorProbeDpi <= 0)
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires positive probe_dpi.").arg(check.id);
                return false;
            }
            if (richBlackPercent < 0.0 || richBlackPercent > 100.0)
            {
                errorMessage = PDFTranslationContext::tr("Check '%1' requires rich_black_k_percent between 0 and 100.").arg(check.id);
                return false;
            }
            check.richBlackKThreshold = richBlackPercent / 100.0;
        }

        const QJsonArray allowedIdentifiers = checkObject.value(QStringLiteral("allowed_identifiers")).toArray();
        for (const QJsonValue& val : allowedIdentifiers)
        {
            check.allowedOutputConditionIdentifiers.append(val.toString());
        }

        if (check.id == QStringLiteral("output-intent"))
        {
            const QJsonArray allowedSubtypes = checkObject.value(QStringLiteral("allowed_subtypes")).toArray();
            for (const QJsonValue& val : allowedSubtypes)
            {
                const QString subtype = val.toString();
                if (!subtype.isEmpty())
                {
                    check.allowedOutputIntentSubtypes.append(subtype);
                }
            }

            const QJsonArray allowedProfileSha256 = checkObject.value(QStringLiteral("allowed_profile_sha256")).toArray();
            for (const QJsonValue& val : allowedProfileSha256)
            {
                const QString digest = val.toString();
                if (digest.size() != 64 || !std::all_of(digest.cbegin(), digest.cend(), [](QChar character) {
                    return character.isDigit() || (character.toLower() >= QLatin1Char('a') && character.toLower() <= QLatin1Char('f'));
                }))
                {
                    errorMessage = PDFTranslationContext::tr("Check '%1' contains an invalid allowed_profile_sha256 digest.").arg(check.id);
                    return false;
                }
                check.allowedOutputIntentProfileSha256.append(digest.toLower());
            }

            check.requireEmbeddedOutputIntentProfile = checkObject.value(QStringLiteral("require_embedded_profile")).toBool(true);
            check.allowMultipleOutputIntents = checkObject.value(QStringLiteral("allow_multiple")).toBool(true);
        }

        profile.checks.push_back(check);
    }

    const QJsonArray fixups = profileObject.value(QStringLiteral("fixups")).toArray();
    for (const QJsonValue& fixupValue : fixups)
    {
        const QJsonObject fixupObject = fixupValue.toObject();
        PreflightFixupConfig fixup;
        fixup.id = fixupObject.value(QStringLiteral("id")).toString();
        if (fixup.id.isEmpty())
        {
            continue;
        }
        if (!isImplementedFixupId(fixup.id))
        {
            errorMessage = PDFTranslationContext::tr("Profile requests unimplemented fixup '%1'.").arg(fixup.id);
            return false;
        }

        fixup.confirm = fixupObject.value(QStringLiteral("confirm")).toBool(true);
        fixup.amountPt = fixupObject.value(QStringLiteral("amount_pt")).toDouble(0.0);
        fixup.description = fixupObject.value(QStringLiteral("description")).toString();
        fixup.params = fixupObject.value(QStringLiteral("params")).toObject();
        if (fixupObject.contains(QStringLiteral("target_dpi"))
            && !fixup.params.contains(QStringLiteral("target_dpi")))
        {
            fixup.params.insert(QStringLiteral("target_dpi"), fixupObject.value(QStringLiteral("target_dpi")));
        }
        if (fixup.id == QStringLiteral("downsample-images"))
        {
            const QJsonValue targetDpiValue = fixup.params.value(QStringLiteral("target_dpi"));
            if (!targetDpiValue.isUndefined()
                && (!targetDpiValue.isDouble()
                    || !std::isfinite(targetDpiValue.toDouble())
                    || std::floor(targetDpiValue.toDouble()) != targetDpiValue.toDouble()
                    || targetDpiValue.toInt() < 72
                    || targetDpiValue.toInt() > 1200))
            {
                errorMessage = PDFTranslationContext::tr(
                    "Fixup 'downsample-images' requires an integral target_dpi between 72 and 1200.");
                return false;
            }
        }

        profile.fixups.push_back(fixup);
    }

    return true;
}

void PreflightEngine::registerBuiltInChecks()
{
    m_checks[QStringLiteral("bleed")] = [](PDFDocumentSession* session,
                                            const PreflightCheckConfig& check,
                                            QList<PreflightFinding>& errors,
                                            QList<PreflightFinding>& warnings)
    {
        runBleedCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("trim")] = [](PDFDocumentSession* session,
                                           const PreflightCheckConfig& check,
                                           QList<PreflightFinding>& errors,
                                           QList<PreflightFinding>& warnings)
    {
        runSizeCheck(SizeCheckKind::Trim, session, check, errors, warnings);
    };

    m_checks[QStringLiteral("page-size")] = [](PDFDocumentSession* session,
                                                 const PreflightCheckConfig& check,
                                                 QList<PreflightFinding>& errors,
                                                 QList<PreflightFinding>& warnings)
    {
        runSizeCheck(SizeCheckKind::PageSize, session, check, errors, warnings);
    };

    m_checks[QStringLiteral("processing-steps")] = [](PDFDocumentSession* session,
                                                       const PreflightCheckConfig& check,
                                                       QList<PreflightFinding>& errors,
                                                       QList<PreflightFinding>& warnings)
    {
        runProcessingStepsCheck(session, check, errors, warnings);
    };
    m_checks[QStringLiteral("dieline")] = m_checks.at(QStringLiteral("processing-steps"));

    m_checks[QStringLiteral("content-bleed")] = [](PDFDocumentSession* session,
                                                     const PreflightCheckConfig& check,
                                                     QList<PreflightFinding>& errors,
                                                     QList<PreflightFinding>& warnings)
    {
        runContentBleedCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("ink-coverage")] = [](PDFDocumentSession* session,
                                                    const PreflightCheckConfig& check,
                                                    QList<PreflightFinding>& errors,
                                                    QList<PreflightFinding>& warnings)
    {
        runInkCoverageCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("color-mode")] = [](PDFDocumentSession* session,
                                                  const PreflightCheckConfig& check,
                                                  QList<PreflightFinding>& errors,
                                                  QList<PreflightFinding>& warnings)
    {
        runColorModeCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("transparency-risk")] = [](PDFDocumentSession* session,
                                                         const PreflightCheckConfig& check,
                                                         QList<PreflightFinding>& errors,
                                                         QList<PreflightFinding>& warnings)
    {
        runTransparencyRiskCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("thin-strokes")] = [](PDFDocumentSession* session,
                                                    const PreflightCheckConfig& check,
                                                    QList<PreflightFinding>& errors,
                                                    QList<PreflightFinding>& warnings)
    {
        runThinStrokesCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("color-inventory")] = [](PDFDocumentSession* session,
                                                       const PreflightCheckConfig& check,
                                                       QList<PreflightFinding>& errors,
                                                       QList<PreflightFinding>& warnings)
    {
        runColorInventoryCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("output-intent")] = [](PDFDocumentSession* session,
                                                     const PreflightCheckConfig& check,
                                                     QList<PreflightFinding>& errors,
                                                     QList<PreflightFinding>& warnings)
    {
        runOutputIntentCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("embedded-fonts")] = [](PDFDocumentSession* session,
                                                     const PreflightCheckConfig& check,
                                                     QList<PreflightFinding>& errors,
                                                     QList<PreflightFinding>& warnings)
    {
        runEmbeddedFontsCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("font-integrity")] = [](PDFDocumentSession* session,
                                                      const PreflightCheckConfig& check,
                                                      QList<PreflightFinding>& errors,
                                                      QList<PreflightFinding>& warnings)
    {
        runFontIntegrityCheck(session, check, errors, warnings);
    };

    for (const QString& id : { QStringLiteral("invisible-content"),
                               QStringLiteral("hidden-layers"),
                               QStringLiteral("off-page-content"),
                               QStringLiteral("obscured-content") })
    {
        m_checks[id] = [](PDFDocumentSession* session,
                          const PreflightCheckConfig& check,
                          QList<PreflightFinding>& errors,
                          QList<PreflightFinding>& warnings)
        {
            runHiddenContentCheck(session, check, errors, warnings);
        };
    }

    m_checks[QStringLiteral("image-resolution")] = [](PDFDocumentSession* session,
                                                       const PreflightCheckConfig& check,
                                                       QList<PreflightFinding>& errors,
                                                       QList<PreflightFinding>& warnings)
    {
        runImageResolutionCheck(session, check, errors, warnings);
    };

    m_checks[QStringLiteral("white-overprint")] = [](PDFDocumentSession* session,
                                                       const PreflightCheckConfig& check,
                                                       QList<PreflightFinding>& errors,
                                                       QList<PreflightFinding>& warnings)
    {
        runWhiteOverprintCheck(session, check, errors, warnings);
    };
}

} // namespace pdf
