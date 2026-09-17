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

#include "pdfobjectselector.h"

#include "pdfcatalog.h"
#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocument.h"
#include "pdfimage.h"
#include "pdfmeshqualitysettings.h"
#include "pdfoptionalcontent.h"
#include "pdfpage.h"
#include "pdfcolorspaces.h"
#include "pdfpagecontentprocessor.h"
#include "pdffont.h"
#include "pdfrenderer.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace pdf
{

namespace
{

constexpr char OBJECT_SELECTOR_SCHEMA[] = "loop-object-selector/1";

void appendPredicateError(QStringList* errors, const QString& message)
{
    if (errors)
    {
        errors->append(message);
    }
}

bool isSha256Digest(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"),
                                          QRegularExpression::CaseInsensitiveOption);
    return pattern.match(value.trimmed()).hasMatch();
}

QString revisionDigestForIdentity(const PDFRevisionIdentity& revision)
{
    return QString::fromLatin1(QCryptographicHash::hash(revision.toString().toUtf8(), QCryptographicHash::Sha256).toHex());
}

const PDFAbstractColorSpace* resolveIndexedBaseColorSpace(const PDFAbstractColorSpace* colorSpace)
{
    if (!colorSpace)
    {
        return nullptr;
    }

    const PDFAbstractColorSpace* base = colorSpace;
    while (base && base->getColorSpace() == PDFAbstractColorSpace::ColorSpace::Indexed)
    {
        base = static_cast<const PDFIndexedColorSpace*>(base)->getBaseColorSpace().get();
    }
    return base;
}

QString classifyColorSpace(const PDFAbstractColorSpace* colorSpace)
{
    const PDFAbstractColorSpace* base = resolveIndexedBaseColorSpace(colorSpace);
    if (!base)
    {
        return QString();
    }

    switch (base->getColorSpace())
    {
        case PDFAbstractColorSpace::ColorSpace::DeviceRGB:
        case PDFAbstractColorSpace::ColorSpace::CalRGB:
            return QStringLiteral("DeviceRGB");
        case PDFAbstractColorSpace::ColorSpace::DeviceCMYK:
            return QStringLiteral("DeviceCMYK");
        case PDFAbstractColorSpace::ColorSpace::DeviceGray:
        case PDFAbstractColorSpace::ColorSpace::CalGray:
            return QStringLiteral("DeviceGray");
        case PDFAbstractColorSpace::ColorSpace::Separation:
            return QStringLiteral("Separation");
        case PDFAbstractColorSpace::ColorSpace::DeviceN:
            return QStringLiteral("DeviceN");
        case PDFAbstractColorSpace::ColorSpace::ICCBased:
            return QStringLiteral("ICCBased");
        case PDFAbstractColorSpace::ColorSpace::Pattern:
            return QStringLiteral("Pattern");
        default:
            return QStringLiteral("Unknown");
    }
}

QString separationColorName(const PDFAbstractColorSpace* colorSpace)
{
    const PDFAbstractColorSpace* base = resolveIndexedBaseColorSpace(colorSpace);
    if (!base || base->getColorSpace() != PDFAbstractColorSpace::ColorSpace::Separation)
    {
        return QString();
    }

    const PDFSeparationColorSpace* separationColorSpace = static_cast<const PDFSeparationColorSpace*>(base);
    return QString::fromLatin1(separationColorSpace->getColorName());
}

void assignColorSpaceMetadata(PDFObjectSelectorCandidate* candidate, const PDFAbstractColorSpace* colorSpace)
{
    if (!candidate)
    {
        return;
    }

    candidate->colorSpaceName = classifyColorSpace(colorSpace);
    candidate->spotColorName = separationColorName(colorSpace);
}

double axisDpi(qreal axisLengthPoints, int pixels)
{
    if (pixels <= 0 || axisLengthPoints <= 0.0)
    {
        return 0.0;
    }
    const double inches = static_cast<double>(axisLengthPoints) * PDF_POINT_TO_INCH;
    return inches > 0.0 ? static_cast<double>(pixels) / inches : 0.0;
}

double effectiveDpiFromImage(const PDFImage& image, const QTransform& ctm)
{
    const int width = image.getImageData().getWidth();
    const int height = image.getImageData().getHeight();
    if (width <= 0 || height <= 0)
    {
        return 0.0;
    }
    const QLineF xAxis(0.0, 0.0, 1.0, 0.0);
    const QLineF yAxis(0.0, 0.0, 0.0, 1.0);
    const QLineF mappedX = ctm.map(xAxis);
    const QLineF mappedY = ctm.map(yAxis);
    const double dpiX = axisDpi(mappedX.length(), width);
    const double dpiY = axisDpi(mappedY.length(), height);
    if (dpiX <= 0.0)
    {
        return dpiY;
    }
    if (dpiY <= 0.0)
    {
        return dpiX;
    }
    return std::min(dpiX, dpiY);
}

QRectF pageBoxRect(const PDFPage* page, const QString& anchor)
{
    if (!page)
    {
        return QRectF();
    }
    if (anchor == QStringLiteral("media"))
    {
        return page->getMediaBox().normalized();
    }
    if (anchor == QStringLiteral("bleed"))
    {
        const QRectF bleed = page->getBleedBox().normalized();
        return bleed.isEmpty() ? page->getCropBox().normalized() : bleed;
    }
    if (anchor == QStringLiteral("crop"))
    {
        return page->getCropBox().normalized();
    }
    return page->getTrimBox().normalized();
}

struct CatalogCandidateKey
{
    int pageIndex = -1;
    PDFObjectReference reference;
    QString objectClass;

    bool operator<(const CatalogCandidateKey& other) const
    {
        return std::tie(pageIndex, reference.objectNumber, reference.generation, objectClass) <
               std::tie(other.pageIndex, other.reference.objectNumber, other.reference.generation, other.objectClass);
    }
};

class PDFObjectSelectorCatalogProcessor final : public PDFPageContentProcessor
{
public:
    PDFObjectSelectorCatalogProcessor(int pageIndex,
                                      const PDFPage* page,
                                      const PDFDocument* document,
                                      const PDFFontCache* fontCache,
                                      const PDFCMS* cms,
                                      const PDFOptionalContentActivity* optionalContentActivity,
                                      std::map<CatalogCandidateKey, PDFObjectSelectorCandidate>* candidates) :
        PDFPageContentProcessor(page, document, fontCache, cms, optionalContentActivity, QTransform(), PDFMeshQualitySettings()),
        m_pageIndex(pageIndex),
        m_candidates(candidates)
    {
    }

protected:
    bool performOriginalImagePainting(const PDFImage& image, const PDFStream* stream, PDFObjectReference reference) override
    {
        Q_UNUSED(stream);
        if (isContentSuppressed() || !reference.isValid())
        {
            return true;
        }

        PDFObjectSelectorCandidate candidate;
        candidate.pageIndex = m_pageIndex;
        candidate.objectReference = reference;
        candidate.objectClass = QStringLiteral("image");
        candidate.isVector = false;
        candidate.effectiveDpi = effectiveDpiFromImage(image, getGraphicState()->getCurrentTransformationMatrix());
        candidate.boundsPt = getGraphicState()->getCurrentTransformationMatrix()
                                 .mapRect(QRectF(0, 0, 1, 1))
                                 .normalized();
        if (const PDFAbstractColorSpace* imageColorSpace = image.getColorSpace().data())
        {
            assignColorSpaceMetadata(&candidate, imageColorSpace);
        }
        else
        {
            assignColorSpaceMetadata(&candidate, getGraphicState()->getFillColorSpace());
        }
        candidate.layerName = currentLayerName();
        candidate.fontName.clear();
        upsertCandidate(candidate);
        return true;
    }

    void performPathPainting(const QPainterPath& path, bool stroke, bool fill, bool text, Qt::FillRule fillRule) override
    {
        Q_UNUSED(fillRule);
        if (isContentSuppressed() || path.isEmpty())
        {
            return;
        }

        PDFObjectSelectorCandidate candidate;
        candidate.pageIndex = m_pageIndex;
        candidate.objectClass = text ? QStringLiteral("text") : QStringLiteral("vector");
        candidate.isVector = !text;
        candidate.boundsPt = path.boundingRect();
        const PDFAbstractColorSpace* colorSpace = stroke ? getGraphicState()->getStrokeColorSpace()
                                                         : getGraphicState()->getFillColorSpace();
        assignColorSpaceMetadata(&candidate, colorSpace);
        candidate.layerName = currentLayerName();
        candidate.effectiveDpi = 0.0;
        upsertCandidate(candidate);
    }

    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (isContentSuppressed())
        {
            return;
        }

        PDFObjectSelectorCandidate candidate;
        candidate.pageIndex = m_pageIndex;
        candidate.objectClass = QStringLiteral("text");
        candidate.isVector = false;
        candidate.boundsPt = info.matrix.mapRect(info.outline.boundingRect());
        assignColorSpaceMetadata(&candidate, getGraphicState()->getFillColorSpace());
        candidate.layerName = currentLayerName();
        const PDFFontPointer& font = getGraphicState()->getTextFont();
        if (font)
        {
            const FontDescriptor* descriptor = font->getFontDescriptor();
            if (descriptor && !descriptor->fontName.isEmpty())
            {
                candidate.fontName = QString::fromLatin1(descriptor->fontName);
            }
        }
        upsertCandidate(candidate);
    }

    void performMarkedContentBegin(const QByteArray& tag, const PDFObject& properties) override
    {
        PDFPageContentProcessor::performMarkedContentBegin(tag, properties);
        if (tag != "OC")
        {
            return;
        }

        QString name;
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
            name = getDocument()->getCatalog()->getOptionalContentProperties()->getOptionalContentGroup(reference).getName();
        }
        if (name.isEmpty())
        {
            name = reference.isValid()
                       ? QStringLiteral("object %1 %2").arg(reference.objectNumber).arg(reference.generation)
                       : QStringLiteral("unnamed optional-content group");
        }
        m_layerStack.push_back(name);
    }

    void performMarkedContentEnd() override
    {
        PDFPageContentProcessor::performMarkedContentEnd();
        if (!m_layerStack.isEmpty())
        {
            m_layerStack.pop_back();
        }
    }

private:
    QString currentLayerName() const
    {
        return m_layerStack.isEmpty() ? QString() : m_layerStack.back();
    }

    void upsertCandidate(const PDFObjectSelectorCandidate& candidate)
    {
        CatalogCandidateKey key{ candidate.pageIndex, candidate.objectReference, candidate.objectClass };
        if (candidate.objectReference.isValid())
        {
            (*m_candidates)[key] = candidate;
            return;
        }

        auto it = m_candidates->find(key);
        if (it == m_candidates->end())
        {
            (*m_candidates)[key] = candidate;
            return;
        }
        it->second.boundsPt = it->second.boundsPt.united(candidate.boundsPt);
        if (candidate.effectiveDpi > 0.0 &&
            (it->second.effectiveDpi <= 0.0 || candidate.effectiveDpi < it->second.effectiveDpi))
        {
            it->second.effectiveDpi = candidate.effectiveDpi;
        }
        if (it->second.colorSpaceName.isEmpty())
        {
            it->second.colorSpaceName = candidate.colorSpaceName;
        }
        if (it->second.spotColorName.isEmpty())
        {
            it->second.spotColorName = candidate.spotColorName;
        }
        if (it->second.fontName.isEmpty())
        {
            it->second.fontName = candidate.fontName;
        }
        if (it->second.layerName.isEmpty())
        {
            it->second.layerName = candidate.layerName;
        }
    }

    int m_pageIndex = -1;
    std::map<CatalogCandidateKey, PDFObjectSelectorCandidate>* m_candidates = nullptr;
    QVector<QString> m_layerStack;
};

QVector<PDFObjectSelectorCandidate> collectCandidates(const PDFDocument& document)
{
    std::map<CatalogCandidateKey, PDFObjectSelectorCandidate> merged;
    const PDFCatalog* catalog = document.getCatalog();
    if (!catalog)
    {
        return {};
    }

    PDFOptionalContentActivity optionalContentActivity(&document, OCUsage::Export, nullptr);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(&document);
    cmsManager.setSettings(cmsManager.getDefaultSettings());
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument modifiedDocument(const_cast<PDFDocument*>(&document), &optionalContentActivity);
    fontCache.setDocument(modifiedDocument);
    fontCache.setCacheShrinkEnabled(nullptr, false);

    const PDFInteger pageCount = catalog->getPageCount();
    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        PDFObjectSelectorCatalogProcessor processor(static_cast<int>(pageIndex),
                                                    page,
                                                    &document,
                                                    &fontCache,
                                                    cms.data(),
                                                    &optionalContentActivity,
                                                    &merged);
        processor.processContents();
    }

    QVector<PDFObjectSelectorCandidate> candidates;
    candidates.reserve(static_cast<int>(merged.size()));
    for (const auto& entry : merged)
    {
        candidates.append(entry.second);
    }
    return candidates;
}

bool parsePredicateObject(const QJsonObject& object,
                          const QMap<QString, QJsonObject>& namedSets,
                          QJsonObject* predicate,
                          QStringList* errors,
                          const QString& path)
{
    if (object.isEmpty())
    {
        appendPredicateError(errors, QStringLiteral("%1 must not be empty.").arg(path));
        return false;
    }

    static const QStringList compositeKeys = { QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("not"), QStringLiteral("set") };
    static const QStringList leafKeys = {
        QStringLiteral("pages"), QStringLiteral("objectClass"), QStringLiteral("colorSpace"),
        QStringLiteral("layer"), QStringLiteral("font"), QStringLiteral("spotColor"),
        QStringLiteral("minEffectiveDpi"), QStringLiteral("maxEffectiveDpi"), QStringLiteral("isVector"),
        QStringLiteral("objectRef"), QStringLiteral("region")
    };

    QString matchedKey;
    for (const QString& key : object.keys())
    {
        if (!matchedKey.isEmpty())
        {
            appendPredicateError(errors, QStringLiteral("%1 must contain exactly one predicate key.").arg(path));
            return false;
        }
        matchedKey = key;
    }

    if (matchedKey == QStringLiteral("set"))
    {
        const QString setName = object.value(QStringLiteral("set")).toString().trimmed();
        if (!namedSets.contains(setName))
        {
            appendPredicateError(errors, QStringLiteral("%1 references unknown set '%2'.").arg(path, setName));
            return false;
        }
        *predicate = namedSets.value(setName);
        return true;
    }

    if (matchedKey == QStringLiteral("not"))
    {
        const QJsonValue child = object.value(QStringLiteral("not"));
        if (!child.isObject())
        {
            appendPredicateError(errors, QStringLiteral("%1.not must be an object.").arg(path));
            return false;
        }
        QJsonObject innerPredicate;
        if (!parsePredicateObject(child.toObject(), namedSets, &innerPredicate, errors, path + QStringLiteral(".not")))
        {
            return false;
        }
        *predicate = QJsonObject{ { QStringLiteral("not"), innerPredicate } };
        return true;
    }

    if (matchedKey == QStringLiteral("and") || matchedKey == QStringLiteral("or"))
    {
        const QJsonArray children = object.value(matchedKey).toArray();
        if (children.isEmpty())
        {
            appendPredicateError(errors, QStringLiteral("%1.%2 must contain at least one predicate.").arg(path, matchedKey));
            return false;
        }
        *predicate = object;
        return true;
    }

    if (!leafKeys.contains(matchedKey))
    {
        appendPredicateError(errors, QStringLiteral("%1 contains unsupported predicate '%2'.").arg(path, matchedKey));
        return false;
    }

    *predicate = object;
    return true;
}

QSet<int> pageIndicesFromSpec(const PDFDocument& document, const QString& pagesSpec, QString* errorMessage)
{
    QSet<int> result;
    const PDFCatalog* catalog = document.getCatalog();
    if (!catalog)
    {
        return result;
    }

    const PDFInteger pageCount = catalog->getPageCount();
    QString parseError;
    const PDFClosedIntervalSet ranges = PDFClosedIntervalSet::parse(1, pageCount, pagesSpec.trimmed(), &parseError);
    if (!parseError.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = parseError;
        }
        return {};
    }

    for (const PDFInteger pageNumber : ranges.unfold())
    {
        result.insert(static_cast<int>(pageNumber - 1));
    }
    return result;
}

bool candidateMatchesLeaf(const PDFObjectSelectorCandidate& candidate,
                          const PDFDocument& document,
                          const QJsonObject& predicate,
                          const PDFPage* page)
{
    const QString key = predicate.begin().key();
    const QJsonValue value = predicate.begin().value();

    if (key == QStringLiteral("pages"))
    {
        QString error;
        const QSet<int> pages = pageIndicesFromSpec(document, value.toString(), &error);
        return error.isEmpty() && pages.contains(candidate.pageIndex);
    }
    if (key == QStringLiteral("objectClass"))
    {
        return candidate.objectClass.compare(value.toString(), Qt::CaseInsensitive) == 0;
    }
    if (key == QStringLiteral("colorSpace"))
    {
        return candidate.colorSpaceName.compare(value.toString(), Qt::CaseInsensitive) == 0;
    }
    if (key == QStringLiteral("layer"))
    {
        return candidate.layerName.compare(value.toString(), Qt::CaseInsensitive) == 0;
    }
    if (key == QStringLiteral("font"))
    {
        return candidate.fontName.contains(value.toString(), Qt::CaseInsensitive);
    }
    if (key == QStringLiteral("spotColor"))
    {
        return candidate.colorSpaceName.compare(QStringLiteral("Separation"), Qt::CaseInsensitive) == 0 &&
               candidate.spotColorName.compare(value.toString(), Qt::CaseInsensitive) == 0;
    }
    if (key == QStringLiteral("minEffectiveDpi"))
    {
        return candidate.effectiveDpi >= value.toDouble();
    }
    if (key == QStringLiteral("maxEffectiveDpi"))
    {
        return candidate.effectiveDpi > 0.0 && candidate.effectiveDpi <= value.toDouble();
    }
    if (key == QStringLiteral("isVector"))
    {
        return candidate.isVector == value.toBool();
    }
    if (key == QStringLiteral("objectRef"))
    {
        const QJsonObject objectRef = value.toObject();
        const PDFObjectReference reference(static_cast<PDFInteger>(objectRef.value(QStringLiteral("object")).toInt()),
                                           static_cast<PDFInteger>(objectRef.value(QStringLiteral("generation")).toInt(0)));
        return candidate.objectReference.isValid() && candidate.objectReference == reference;
    }
    if (key == QStringLiteral("region"))
    {
        const QJsonObject region = value.toObject();
        const QJsonArray rectArray = region.value(QStringLiteral("rect_pt")).toArray();
        if (rectArray.size() != 4 || !page)
        {
            return false;
        }
        const QRectF regionRect(rectArray.at(0).toDouble(),
                                rectArray.at(1).toDouble(),
                                rectArray.at(2).toDouble(),
                                rectArray.at(3).toDouble());
        const QString anchor = region.value(QStringLiteral("anchor")).toString(QStringLiteral("trim"));
        const QString mode = region.value(QStringLiteral("mode")).toString(QStringLiteral("include"));
        const QRectF anchorRect = pageBoxRect(page, anchor);
        const QRectF absolute = regionRect.translated(anchorRect.topLeft());
        const bool intersects = candidate.boundsPt.intersects(absolute);
        return mode == QStringLiteral("exclude") ? !intersects : intersects;
    }
    return false;
}

bool evaluatePredicate(const PDFObjectSelectorCandidate& candidate,
                       const PDFDocument& document,
                       const QJsonObject& predicate,
                       const QMap<QString, QJsonObject>& namedSets)
{
    if (predicate.contains(QStringLiteral("and")))
    {
        for (const QJsonValue& child : predicate.value(QStringLiteral("and")).toArray())
        {
            if (!evaluatePredicate(candidate, document, child.toObject(), namedSets))
            {
                return false;
            }
        }
        return true;
    }
    if (predicate.contains(QStringLiteral("or")))
    {
        for (const QJsonValue& child : predicate.value(QStringLiteral("or")).toArray())
        {
            if (evaluatePredicate(candidate, document, child.toObject(), namedSets))
            {
                return true;
            }
        }
        return false;
    }
    if (predicate.contains(QStringLiteral("not")))
    {
        return !evaluatePredicate(candidate, document, predicate.value(QStringLiteral("not")).toObject(), namedSets);
    }
    if (predicate.contains(QStringLiteral("set")))
    {
        const QString setName = predicate.value(QStringLiteral("set")).toString().trimmed();
        return evaluatePredicate(candidate, document, namedSets.value(setName), namedSets);
    }

    const PDFPage* page = document.getCatalog() ? document.getCatalog()->getPage(candidate.pageIndex) : nullptr;
    return candidateMatchesLeaf(candidate, document, predicate, page);
}

bool candidateMatchesSelection(const PDFObjectSelectorCandidate& candidate, const PDFObjectSelectionResult& selection)
{
    for (const PDFObjectSelectorCandidate& allowed : selection.candidates)
    {
        if (allowed.objectReference.isValid() && candidate.objectReference.isValid())
        {
            if (allowed.objectReference == candidate.objectReference)
            {
                return true;
            }
            continue;
        }
        if (allowed.pageIndex == candidate.pageIndex &&
            allowed.objectClass == candidate.objectClass &&
            !allowed.objectReference.isValid() &&
            !candidate.objectReference.isValid())
        {
            return true;
        }
    }
    return false;
}

}   // namespace

QString PDFObjectSelectorCandidate::stableId() const
{
    if (objectReference.isValid())
    {
        return QStringLiteral("obj:%1:%2:%3")
            .arg(pageIndex + 1)
            .arg(objectReference.objectNumber)
            .arg(objectReference.generation);
    }
    return QStringLiteral("page:%1:%2").arg(pageIndex + 1).arg(objectClass);
}

QJsonObject PDFObjectSelectorCandidate::toJson() const
{
    QJsonObject result{
        { QStringLiteral("id"), stableId() },
        { QStringLiteral("page"), pageIndex },
        { QStringLiteral("class"), objectClass }
    };
    if (objectReference.isValid())
    {
        result.insert(QStringLiteral("object"), QStringLiteral("%1 %2 R")
                                                   .arg(objectReference.objectNumber)
                                                   .arg(objectReference.generation));
    }
    if (!colorSpaceName.isEmpty())
    {
        result.insert(QStringLiteral("color_space"), colorSpaceName);
    }
    if (!spotColorName.isEmpty())
    {
        result.insert(QStringLiteral("spot_color"), spotColorName);
    }
    if (!layerName.isEmpty())
    {
        result.insert(QStringLiteral("layer"), layerName);
    }
    if (!fontName.isEmpty())
    {
        result.insert(QStringLiteral("font"), fontName);
    }
    if (effectiveDpi > 0.0)
    {
        result.insert(QStringLiteral("effective_dpi"), effectiveDpi);
    }
    if (isVector)
    {
        result.insert(QStringLiteral("is_vector"), true);
    }
    return result;
}

PDFRepairTarget PDFObjectSelectorCandidate::toRepairTarget() const
{
    return { pageIndex, objectReference, QStringLiteral("selection/%1").arg(stableId()) };
}

QJsonObject PDFObjectSelectionResult::previewScope() const
{
    QJsonArray candidatesJson;
    for (const PDFObjectSelectorCandidate& candidate : candidates)
    {
        candidatesJson.append(candidate.toJson());
    }
    return QJsonObject{
        { QStringLiteral("count"), candidates.size() },
        { QStringLiteral("digest"), digest },
        { QStringLiteral("empty"), empty },
        { QStringLiteral("candidates"), candidatesJson }
    };
}

QJsonObject PDFObjectSelectionResult::toJson() const
{
    return QJsonObject{
        { QStringLiteral("ok"), ok },
        { QStringLiteral("empty"), empty },
        { QStringLiteral("stale_revision"), staleRevision },
        { QStringLiteral("error"), errorMessage },
        { QStringLiteral("digest"), digest },
        { QStringLiteral("revision"), resolvedRevision.toString() },
        { QStringLiteral("preview"), previewScope() }
    };
}

QString PDFObjectSelector::schemaVersion()
{
    return QString::fromLatin1(OBJECT_SELECTOR_SCHEMA);
}

PDFOperationResult PDFObjectSelector::fromJson(const QJsonObject& object, PDFObjectSelector* result, QStringList* errors)
{
    if (!result)
    {
        return PDFOperationResult(QStringLiteral("Object selector result is null."));
    }

    PDFObjectSelector parsed;
    parsed.m_schema = object.value(QStringLiteral("schema")).toString().trimmed();
    if (parsed.m_schema != schemaVersion())
    {
        appendPredicateError(errors, QStringLiteral("Unsupported object selector schema '%1'.").arg(parsed.m_schema));
        return PDFOperationResult(QStringLiteral("Unsupported object selector schema."));
    }

    parsed.m_revisionDigest = object.value(QStringLiteral("revisionDigest")).toString().trimmed().toLower();
    if (!parsed.m_revisionDigest.isEmpty() && !isSha256Digest(parsed.m_revisionDigest))
    {
        appendPredicateError(errors, QStringLiteral("revisionDigest must be a lowercase SHA-256 digest."));
        return PDFOperationResult(QStringLiteral("Invalid object selector revisionDigest."));
    }

    const QJsonObject setsObject = object.value(QStringLiteral("sets")).toObject();
    for (auto it = setsObject.begin(); it != setsObject.end(); ++it)
    {
        if (!it.value().isObject())
        {
            appendPredicateError(errors, QStringLiteral("Named set '%1' must be an object.").arg(it.key()));
            return PDFOperationResult(QStringLiteral("Invalid named set."));
        }
        parsed.m_namedSets.insert(it.key(), it.value().toObject());
    }

    const QJsonObject predicateObject = object.value(QStringLiteral("predicate")).toObject();
    if (predicateObject.isEmpty())
    {
        appendPredicateError(errors, QStringLiteral("Object selector predicate is required."));
        return PDFOperationResult(QStringLiteral("Missing object selector predicate."));
    }
    if (!parsePredicateObject(predicateObject, parsed.m_namedSets, &parsed.m_predicate, errors, QStringLiteral("predicate")))
    {
        return PDFOperationResult(QStringLiteral("Invalid object selector predicate."));
    }

    *result = std::move(parsed);
    return PDFOperationResult(true);
}

QJsonObject PDFObjectSelector::toJson() const
{
    QJsonObject result{
        { QStringLiteral("schema"), m_schema },
        { QStringLiteral("predicate"), m_predicate }
    };
    if (!m_revisionDigest.isEmpty())
    {
        result.insert(QStringLiteral("revisionDigest"), m_revisionDigest);
    }
    if (!m_namedSets.isEmpty())
    {
        QJsonObject setsObject;
        for (auto it = m_namedSets.begin(); it != m_namedSets.end(); ++it)
        {
            setsObject.insert(it.key(), it.value());
        }
        result.insert(QStringLiteral("sets"), setsObject);
    }
    return result;
}

PDFOperationResult PDFObjectSelector::resolve(const PDFObjectSelector& selector,
                                              const PDFDocument& document,
                                              const PDFRevisionIdentity& revision,
                                              PDFObjectSelectionResult* result)
{
    if (!result)
    {
        return PDFOperationResult(QStringLiteral("Object selection result is null."));
    }

    *result = PDFObjectSelectionResult();
    result->resolvedRevision = revision;

    if (!selector.m_revisionDigest.isEmpty())
    {
        const QString currentDigest = revisionDigestForIdentity(revision);
        if (selector.m_revisionDigest.compare(currentDigest, Qt::CaseInsensitive) != 0)
        {
            result->staleRevision = true;
            result->errorMessage = QStringLiteral("Object selector revisionDigest does not match the active document revision.");
            return PDFOperationResult(result->errorMessage);
        }
    }

    const QVector<PDFObjectSelectorCandidate> catalog = collectCandidates(document);
    QVector<PDFObjectSelectorCandidate> matched;
    matched.reserve(catalog.size());
    for (const PDFObjectSelectorCandidate& candidate : catalog)
    {
        if (evaluatePredicate(candidate, document, selector.m_predicate, selector.m_namedSets))
        {
            matched.append(candidate);
        }
    }

    std::sort(matched.begin(), matched.end(), [](const PDFObjectSelectorCandidate& left, const PDFObjectSelectorCandidate& right)
              { return left.stableId() < right.stableId(); });
    matched.erase(std::unique(matched.begin(), matched.end(),
                              [](const PDFObjectSelectorCandidate& left, const PDFObjectSelectorCandidate& right)
                              { return left.stableId() == right.stableId(); }),
                  matched.end());

    result->candidates = matched;
    result->digest = selectionDigestForCandidates(result->candidates);
    result->empty = result->candidates.isEmpty();
    result->ok = true;
    return PDFOperationResult(true);
}

PDFRevisionIdentity revisionIdentityForDocument(const PDFDocument& document,
                                                DocumentRevision documentRevision,
                                                quint64 cacheGeneration)
{
    PDFRevisionIdentity revision;
    revision.document = PDFDocumentIdentity::fromDocument(&document);
    revision.documentRevision = documentRevision;
    revision.cacheGeneration = cacheGeneration;
    return revision;
}

QString selectionDigestForCandidates(const QVector<PDFObjectSelectorCandidate>& candidates)
{
    QJsonArray canonical;
    for (const PDFObjectSelectorCandidate& candidate : candidates)
    {
        canonical.append(candidate.stableId());
    }
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(canonical).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}

bool filterRepairPlanTargets(PDFRepairPlan* plan, const PDFObjectSelectionResult& selection)
{
    if (!plan || !selection.ok)
    {
        return false;
    }
    if (selection.empty)
    {
        plan->targets.clear();
        return true;
    }

    QList<PDFRepairTarget> filtered;
    for (const PDFRepairTarget& target : plan->targets)
    {
        bool allowed = false;
        for (const PDFObjectSelectorCandidate& candidate : selection.candidates)
        {
            if (target.objectReference.isValid() && candidate.objectReference.isValid())
            {
                allowed = target.objectReference == candidate.objectReference;
            }
            else if (target.pageIndex >= 0 && target.pageIndex == candidate.pageIndex && !target.objectReference.isValid())
            {
                allowed = true;
            }
            if (allowed)
            {
                break;
            }
        }
        if (allowed)
        {
            filtered.append(target);
        }
    }
    plan->targets = filtered;
    return true;
}

bool repairChangesWithinSelection(const QList<PDFRepairChange>& changes,
                                  const PDFObjectSelectionResult& selection,
                                  QString* errorMessage)
{
    if (!selection.ok)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Selection scope is not resolved.");
        }
        return false;
    }
    if (selection.empty)
    {
        return changes.isEmpty();
    }

    for (const PDFRepairChange& change : changes)
    {
        PDFObjectSelectorCandidate probe;
        probe.pageIndex = change.target.pageIndex;
        probe.objectReference = change.target.objectReference;
        probe.objectClass = change.changeKind;
        if (!candidateMatchesSelection(probe, selection))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Repair touched object outside the resolved selection scope.");
            }
            return false;
        }
    }
    return true;
}

}   // namespace pdf
