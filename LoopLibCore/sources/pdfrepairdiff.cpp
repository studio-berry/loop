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

#include "pdfrepairdiff.h"

#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdffont.h"
#include "pdfoptionalcontent.h"
#include "pdfrenderer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace pdf
{

namespace
{

struct PageSnapshot
{
    QRectF mediaBox;
    QRectF cropBox;
    QRectF bleedBox;
    QRectF trimBox;
    QRectF artBox;
    PageRotation rotation = PageRotation::None;
    QStringList annotationTypes;
    QString contentDigest;
    QString resourcesDigest;
    QString fontsDigest;
    QString imagesDigest;
    QString colorSpacesDigest;
};

struct DocumentSnapshot
{
    int pageCount = 0;
    QVector<PageSnapshot> pages;
    QStringList outputIntents;
    QString metadataDigest;
    int annotationCount = 0;
    int embeddedFileCount = 0;
    bool hasSignatures = false;
};

QString rectName(const QRectF& rect)
{
    return QStringLiteral("[%1,%2,%3,%4]")
        .arg(rect.left(), 0, 'f', 4)
        .arg(rect.top(), 0, 'f', 4)
        .arg(rect.width(), 0, 'f', 4)
        .arg(rect.height(), 0, 'f', 4);
}

QString pageRotationName(PageRotation rotation)
{
    switch (rotation)
    {
        case PageRotation::None: return QStringLiteral("0");
        case PageRotation::Rotate90: return QStringLiteral("90");
        case PageRotation::Rotate180: return QStringLiteral("180");
        case PageRotation::Rotate270: return QStringLiteral("270");
    }
    return QStringLiteral("unknown");
}

QString objectTypeName(PDFObject::Type type)
{
    switch (type)
    {
        case PDFObject::Type::Null: return QStringLiteral("null");
        case PDFObject::Type::Bool: return QStringLiteral("bool");
        case PDFObject::Type::Int: return QStringLiteral("int");
        case PDFObject::Type::Real: return QStringLiteral("real");
        case PDFObject::Type::String: return QStringLiteral("string");
        case PDFObject::Type::Name: return QStringLiteral("name");
        case PDFObject::Type::Array: return QStringLiteral("array");
        case PDFObject::Type::Dictionary: return QStringLiteral("dictionary");
        case PDFObject::Type::Stream: return QStringLiteral("stream");
        case PDFObject::Type::Reference: return QStringLiteral("reference");
        case PDFObject::Type::LastType: break;
    }
    return QStringLiteral("unknown");
}

QByteArray digest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray normalizedObject(const PDFDocument& document,
                           const PDFObject& object,
                           std::set<PDFObjectReference>& activeReferences,
                           int depth = 0);

QByteArray normalizedDictionary(const PDFDocument& document,
                                const PDFDictionary* dictionary,
                                std::set<PDFObjectReference>& activeReferences,
                                int depth)
{
    if (!dictionary || depth > 64)
    {
        return QByteArrayLiteral("dictionary-depth-limit");
    }

    struct Entry
    {
        QByteArray key;
        QByteArray value;
    };

    std::vector<Entry> entries;
    entries.reserve(dictionary->getCount());
    for (size_t i = 0; i < dictionary->getCount(); ++i)
    {
        const QByteArray key = dictionary->getKey(i).getString();
        // These keys describe serialization rather than document semantics.
        if (key == "Length" || key == "Filter" || key == "DecodeParms" || key == "DL")
        {
            continue;
        }
        entries.push_back({ key, normalizedObject(document, dictionary->getValue(i), activeReferences, depth + 1) });
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right)
    {
        return left.key < right.key;
    });

    QByteArray result("dict{");
    for (const Entry& entry : entries)
    {
        result += entry.key.toHex();
        result += '=';
        result += entry.value.toHex();
        result += ';';
    }
    result += '}';
    return result;
}

QByteArray normalizedObject(const PDFDocument& document,
                           const PDFObject& inputObject,
                           std::set<PDFObjectReference>& activeReferences,
                           int depth)
{
    if (depth > 64)
    {
        return QByteArrayLiteral("depth-limit");
    }

    if (inputObject.isReference())
    {
        const PDFObjectReference reference = inputObject.getReference();
        if (activeReferences.contains(reference))
        {
            return QByteArrayLiteral("cycle");
        }
        activeReferences.insert(reference);
        const QByteArray result = normalizedObject(document, document.getObjectByReference(reference), activeReferences, depth + 1);
        activeReferences.erase(reference);
        return result;
    }

    if (inputObject.isNull()) return QByteArrayLiteral("null");
    if (inputObject.isBool()) return inputObject.getBool() ? QByteArrayLiteral("true") : QByteArrayLiteral("false");
    if (inputObject.isInt()) return QByteArray("int:") + QByteArray::number(inputObject.getInteger());
    if (inputObject.isReal()) return QByteArray("real:") + QByteArray::number(inputObject.getReal(), 'g', 17);
    if (inputObject.isString()) return QByteArray("string:") + inputObject.getString().toHex();
    if (inputObject.isName()) return QByteArray("name:") + inputObject.getString().toHex();
    if (inputObject.isArray())
    {
        QByteArray result("array[");
        for (const PDFObject& item : *inputObject.getArray())
        {
            result += normalizedObject(document, item, activeReferences, depth + 1).toHex();
            result += ';';
        }
        result += ']';
        return result;
    }
    if (inputObject.isDictionary())
    {
        return normalizedDictionary(document, inputObject.getDictionary(), activeReferences, depth);
    }
    if (inputObject.isStream())
    {
        const PDFStream* stream = inputObject.getStream();
        QByteArray result("stream:");
        result += normalizedDictionary(document, stream->getDictionary(), activeReferences, depth + 1).toHex();
        result += ':';
        result += digest(document.getDecodedStream(stream));
        return result;
    }

    return objectTypeName(inputObject.getType()).toLatin1();
}

QString objectDigest(const PDFDocument& document, const PDFObject& object)
{
    std::set<PDFObjectReference> activeReferences;
    return QString::fromLatin1(digest(normalizedObject(document, object, activeReferences)));
}

QString objectName(const PDFDocument& document, const PDFObject& object)
{
    const PDFObject resolved = document.getObject(object);
    if (resolved.isName() || resolved.isString())
    {
        return QString::fromLatin1(resolved.getString());
    }
    return QString();
}

QStringList annotationTypes(const PDFDocument& document, const PDFPage* page)
{
    QStringList result;
    for (const PDFObjectReference reference : page->getAnnotations())
    {
        const PDFDictionary* dictionary = document.getDictionaryFromObject(document.getObjectByReference(reference));
        const QString subtype = dictionary ? objectName(document, dictionary->get("Subtype")) : QStringLiteral("unknown");
        result.append(subtype.isEmpty() ? QStringLiteral("unknown") : subtype);
    }
    std::sort(result.begin(), result.end());
    return result;
}

QString metadataDigest(const PDFDocument& document)
{
    const PDFDocumentInfo* info = document.getInfo();
    QByteArray serialized;
    serialized += info->title.toUtf8();
    serialized += '\n';
    serialized += info->author.toUtf8();
    serialized += '\n';
    serialized += info->subject.toUtf8();
    serialized += '\n';
    serialized += info->keywords.toUtf8();
    serialized += '\n';
    serialized += info->creator.toUtf8();
    serialized += '\n';
    serialized += info->producer.toUtf8();
    serialized += '\n';
    serialized += info->creationDate.toUTC().toString(Qt::ISODateWithMs).toUtf8();
    serialized += '\n';
    serialized += info->modifiedDate.toUTC().toString(Qt::ISODateWithMs).toUtf8();
    for (const auto& extra : info->extra)
    {
        serialized += '\n';
        serialized += extra.first;
        serialized += '=';
        serialized += extra.second.toString().toUtf8();
    }
    return QString::fromLatin1(digest(serialized));
}

QStringList outputIntentIdentities(const PDFDocument& document)
{
    QStringList result;
    for (const PDFOutputIntent& intent : document.getCatalog()->getOutputIntents())
    {
        result.append(intent.getOutputConditionIdentifier() + QLatin1Char('|') + intent.getOutputCondition()
                      + QLatin1Char('|') + objectDigest(document, intent.getOutputProfile()));
    }
    std::sort(result.begin(), result.end());
    return result;
}

DocumentSnapshot snapshot(const PDFDocument& document)
{
    DocumentSnapshot result;
    result.pageCount = static_cast<int>(document.getCatalog()->getPageCount());
    result.pages.reserve(result.pageCount);

    for (int pageIndex = 0; pageIndex < result.pageCount; ++pageIndex)
    {
        const PDFPage* page = document.getCatalog()->getPage(static_cast<size_t>(pageIndex));
        PageSnapshot pageSnapshot;
        pageSnapshot.mediaBox = page->getMediaBox();
        pageSnapshot.cropBox = page->getCropBox();
        pageSnapshot.bleedBox = page->getBleedBox();
        pageSnapshot.trimBox = page->getTrimBox();
        pageSnapshot.artBox = page->getArtBox();
        pageSnapshot.rotation = page->getPageRotation();
        pageSnapshot.annotationTypes = annotationTypes(document, page);
        pageSnapshot.contentDigest = objectDigest(document, page->getContents());
        pageSnapshot.resourcesDigest = objectDigest(document, page->getResources());

        const PDFDictionary* resources = document.getDictionaryFromObject(document.getObject(page->getResources()));
        if (resources)
        {
            pageSnapshot.fontsDigest = objectDigest(document, resources->get("Font"));
            pageSnapshot.imagesDigest = objectDigest(document, resources->get("XObject"));
            pageSnapshot.colorSpacesDigest = objectDigest(document, resources->get("ColorSpace"));
        }
        result.pages.append(std::move(pageSnapshot));
    }

    result.outputIntents = outputIntentIdentities(document);
    result.metadataDigest = metadataDigest(document);
    for (const PageSnapshot& page : result.pages)
    {
        result.annotationCount += page.annotationTypes.size();
    }

    for (const PDFObjectStorage::Entry& entry : document.getStorage().getObjects())
    {
        const PDFDictionary* dictionary = document.getDictionaryFromObject(entry.object);
        if (!dictionary)
        {
            continue;
        }

        const QString type = objectName(document, dictionary->get("Type"));
        if (type == QStringLiteral("Sig"))
        {
            result.hasSignatures = true;
        }
        if (type == QStringLiteral("EmbeddedFile"))
        {
            ++result.embeddedFileCount;
        }
    }
    return result;
}

PDFRepairChangeClass classify(const QString& kind,
                              const QString& path,
                              const PDFRepairDiffOptions& options)
{
    bool expected = false;
    if (kind == QStringLiteral("page_box")) expected = options.expected.pageBoxes;
    else if (kind == QStringLiteral("content")) expected = options.expected.pageContent;
    else if (kind == QStringLiteral("image")) expected = options.expected.images;
    else if (kind == QStringLiteral("font")) expected = options.expected.fonts;
    else if (kind == QStringLiteral("color_space")) expected = options.expected.colorSpaces;
    else if (kind == QStringLiteral("output_intent")) expected = options.expected.outputIntent;
    else if (kind == QStringLiteral("metadata")) expected = options.expected.metadata;
    else if (kind == QStringLiteral("annotation")) expected = options.expected.annotations;
    else if (kind == QStringLiteral("signature")) expected = options.expected.signatures;
    else if (kind == QStringLiteral("page_count") || kind == QStringLiteral("page_order")) return PDFRepairChangeClass::Unexpected;
    else return PDFRepairChangeClass::Informational;

    if (!options.affectedPages.isEmpty() && path.startsWith(QStringLiteral("pages/")))
    {
        const int slash = path.indexOf(QLatin1Char('/'), 6);
        const int pageIndex = path.mid(6, slash - 6).toInt();
        if (!options.affectedPages.contains(pageIndex))
        {
            return PDFRepairChangeClass::Unexpected;
        }
    }
    return expected ? PDFRepairChangeClass::Expected : PDFRepairChangeClass::Unexpected;
}

QRectF commonRegion(const PDFPage* before, const PDFPage* after)
{
    const QRectF left = before->getCropBox();
    const QRectF right = after->getCropBox();
    const double x1 = std::max(left.left(), right.left());
    const double y1 = std::max(left.top(), right.top());
    const double x2 = std::min(left.right(), right.right());
    const double y2 = std::min(left.bottom(), right.bottom());
    return QRectF(QPointF(x1, y1), QPointF(x2, y2)).normalized();
}

QImage renderPage(const PDFDocument& document, int pageIndex, const QRectF& region, int dpi, QStringList* warnings)
{
    const int width = qCeil(region.width() * dpi / 72.0);
    const int height = qCeil(region.height() * dpi / 72.0);
    if (width <= 0 || height <= 0)
    {
        warnings->append(QStringLiteral("invalid-render-size"));
        return QImage();
    }

    PDFOptionalContentActivity optionalContentActivity(&document, OCUsage::Export, nullptr);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(&document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    fontCache.setDocument(PDFModifiedDocument(const_cast<PDFDocument*>(&document), &optionalContentActivity));
    fontCache.setCacheShrinkEnabled(nullptr, false);

    PDFRenderer renderer(&document,
                         &fontCache,
                         cms.get(),
                         &optionalContentActivity,
                         PDFRenderer::getDefaultFeatures(),
                         PDFMeshQualitySettings());

    QImage image(QSize(width, height), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    const PDFPage* page = document.getCatalog()->getPage(static_cast<size_t>(pageIndex));
    const QTransform transform = PDFRenderer::createMediaBoxToDevicePointMatrix(region,
                                                                                    QRectF(0, 0, width, height),
                                                                                    page->getPageRotation());
    const QList<PDFRenderError> errors = renderer.render(&painter, transform, static_cast<size_t>(pageIndex));
    painter.end();
    for (const PDFRenderError& error : errors)
    {
        warnings->append(error.message);
    }
    return image;
}

QString imageFingerprint(const QImage& image)
{
    const QByteArray imageData(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
    return QString::fromLatin1(QCryptographicHash::hash(imageData, QCryptographicHash::Sha256).toHex());
}

bool insideAllowedRegion(const QVector<PDFRepairAllowedRegion>& regions, int pageIndex, const QPointF& point)
{
    for (const PDFRepairAllowedRegion& region : regions)
    {
        if (region.pageIndex == pageIndex && region.pageRect.contains(point))
        {
            return true;
        }
    }
    return false;
}

PDFRepairPageVisualDiff compareImages(const QImage& before,
                                      const QImage& after,
                                      const QRectF& pageRegion,
                                      const PDFRepairDiffOptions& options,
                                      int pageIndex)
{
    PDFRepairPageVisualDiff result;
    result.pageIndex = pageIndex;
    result.pixelSize = before.size();
    result.beforeFingerprint = imageFingerprint(before);
    result.afterFingerprint = imageFingerprint(after);

    QImage diff(before.size(), QImage::Format_ARGB32_Premultiplied);
    diff.fill(Qt::white);
    quint64 absoluteDelta = 0;
    for (int y = 0; y < before.height(); ++y)
    {
        for (int x = 0; x < before.width(); ++x)
        {
            const QColor left = before.pixelColor(x, y);
            const QColor right = after.pixelColor(x, y);
            const int dr = qAbs(left.red() - right.red());
            const int dg = qAbs(left.green() - right.green());
            const int db = qAbs(left.blue() - right.blue());
            const int da = qAbs(left.alpha() - right.alpha());
            const int maxDelta = std::max({ dr, dg, db, da });
            absoluteDelta += static_cast<quint64>(dr + dg + db + da);
            result.maxChannelDelta = std::max(result.maxChannelDelta, maxDelta);
            if (maxDelta > 0)
            {
                ++result.strictChangedPixelCount;
            }
            if (maxDelta > options.channelTolerance)
            {
                ++result.changedPixelCount;
                result.changedPixelBounds |= QRect(x, y, 1, 1);
                const QPointF pagePoint(pageRegion.left() + (x + 0.5) * pageRegion.width() / before.width(),
                                         pageRegion.top() + (y + 0.5) * pageRegion.height() / before.height());
                if (!options.allowedRegions.isEmpty() && !insideAllowedRegion(options.allowedRegions, pageIndex, pagePoint))
                {
                    ++result.unexpectedChangedPixelCount;
                }
                diff.setPixelColor(x, y, QColor(220, 30, 30, std::min(255, 40 + maxDelta * 4)));
            }
        }
    }
    const quint64 totalPixels = static_cast<quint64>(before.width()) * static_cast<quint64>(before.height());
    result.changedPixelRatio = totalPixels == 0 ? 0.0 : static_cast<double>(result.changedPixelCount) / totalPixels;
    result.meanAbsoluteDelta = totalPixels == 0 ? 0.0 : static_cast<double>(absoluteDelta) / (4.0 * totalPixels);
    if (result.unexpectedChangedPixelCount > 0)
    {
        result.warnings.append(QStringLiteral("changed-pixels-outside-allowed-regions"));
    }
    if (!options.renderDirectory.isEmpty())
    {
        const QString prefix = QStringLiteral("page-%1").arg(pageIndex + 1, 4, 10, QLatin1Char('0'));
        result.beforeImagePath = prefix + QStringLiteral("-before.png");
        result.afterImagePath = prefix + QStringLiteral("-after.png");
        result.diffImagePath = prefix + QStringLiteral("-diff.png");
        before.save(QDir(options.renderDirectory).filePath(result.beforeImagePath), "PNG");
        after.save(QDir(options.renderDirectory).filePath(result.afterImagePath), "PNG");
        diff.save(QDir(options.renderDirectory).filePath(result.diffImagePath), "PNG");
    }
    return result;
}

void addChange(PDFRepairDiffReport* report,
               const QString& path,
               const QString& kind,
               const QString& beforeValue,
               const QString& afterValue,
               const PDFRepairDiffOptions& options)
{
    PDFRepairStructuralChange change;
    change.path = path;
    change.kind = kind;
    change.beforeValue = beforeValue;
    change.afterValue = afterValue;
    change.classification = classify(kind, path, options);
    report->structuralChanges.append(std::move(change));
}

} // namespace

QString pdfRepairDiffStatusName(PDFRepairDiffStatus status)
{
    switch (status)
    {
        case PDFRepairDiffStatus::Complete: return QStringLiteral("complete");
        case PDFRepairDiffStatus::CompleteWithWarnings: return QStringLiteral("complete-with-warnings");
        case PDFRepairDiffStatus::Incomplete: return QStringLiteral("incomplete");
        case PDFRepairDiffStatus::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QString pdfRepairChangeClassName(PDFRepairChangeClass changeClass)
{
    switch (changeClass)
    {
        case PDFRepairChangeClass::Expected: return QStringLiteral("expected");
        case PDFRepairChangeClass::Unexpected: return QStringLiteral("unexpected");
        case PDFRepairChangeClass::Informational: return QStringLiteral("informational");
    }
    return QStringLiteral("informational");
}

QJsonObject PDFRepairDiffReport::toJson() const
{
    QJsonArray pagesJson;
    for (const PDFRepairPageVisualDiff& page : pages)
    {
        pagesJson.append(QJsonObject{
            { QStringLiteral("page_index"), page.pageIndex },
            { QStringLiteral("pixel_width"), page.pixelSize.width() },
            { QStringLiteral("pixel_height"), page.pixelSize.height() },
            { QStringLiteral("before_fingerprint"), page.beforeFingerprint },
            { QStringLiteral("after_fingerprint"), page.afterFingerprint },
            { QStringLiteral("strict_changed_pixel_count"), static_cast<qint64>(page.strictChangedPixelCount) },
            { QStringLiteral("changed_pixel_count"), static_cast<qint64>(page.changedPixelCount) },
            { QStringLiteral("unexpected_changed_pixel_count"), static_cast<qint64>(page.unexpectedChangedPixelCount) },
            { QStringLiteral("changed_pixel_ratio"), page.changedPixelRatio },
            { QStringLiteral("mean_absolute_delta"), page.meanAbsoluteDelta },
            { QStringLiteral("max_channel_delta"), page.maxChannelDelta },
            { QStringLiteral("changed_bounds"), QJsonObject{
                  { QStringLiteral("left"), page.changedPixelBounds.left() },
                  { QStringLiteral("top"), page.changedPixelBounds.top() },
                  { QStringLiteral("width"), page.changedPixelBounds.width() },
                  { QStringLiteral("height"), page.changedPixelBounds.height() } } },
            { QStringLiteral("common_region_compared"), page.commonRegionCompared },
            { QStringLiteral("warnings"), QJsonArray::fromStringList(page.warnings) },
            { QStringLiteral("artifacts"), QJsonObject{
                  { QStringLiteral("before"), page.beforeImagePath },
                  { QStringLiteral("after"), page.afterImagePath },
                  { QStringLiteral("diff"), page.diffImagePath } } }
        });
    }

    QJsonArray changesJson;
    for (const PDFRepairStructuralChange& change : structuralChanges)
    {
        changesJson.append(QJsonObject{
            { QStringLiteral("path"), change.path },
            { QStringLiteral("kind"), change.kind },
            { QStringLiteral("before"), change.beforeValue },
            { QStringLiteral("after"), change.afterValue },
            { QStringLiteral("classification"), pdfRepairChangeClassName(change.classification) }
        });
    }

    int expectedCount = 0;
    int unexpectedCount = 0;
    for (const PDFRepairStructuralChange& change : structuralChanges)
    {
        expectedCount += change.classification == PDFRepairChangeClass::Expected;
        unexpectedCount += change.classification == PDFRepairChangeClass::Unexpected;
    }

    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.repair-diff") },
        { QStringLiteral("version"), schemaVersion },
        { QStringLiteral("status"), pdfRepairDiffStatusName(status) },
        { QStringLiteral("source"), QJsonObject{{ QStringLiteral("sha256"), sourceFingerprint }} },
        { QStringLiteral("candidate"), QJsonObject{{ QStringLiteral("sha256"), candidateFingerprint }} },
        { QStringLiteral("summary"), QJsonObject{
              { QStringLiteral("pages_compared"), pages.size() },
              { QStringLiteral("pages_visually_changed"), std::count_if(pages.cbegin(), pages.cend(), [](const PDFRepairPageVisualDiff& page) { return page.changedPixelCount > 0; }) },
              { QStringLiteral("expected_structural_changes"), expectedCount },
              { QStringLiteral("unexpected_structural_changes"), unexpectedCount },
              { QStringLiteral("incomplete_checks"), incompleteReasons.size() } } },
        { QStringLiteral("pages"), pagesJson },
        { QStringLiteral("structural_changes"), changesJson },
        { QStringLiteral("warnings"), QJsonArray::fromStringList(warnings) },
        { QStringLiteral("incomplete_reasons"), QJsonArray::fromStringList(incompleteReasons) }
    };
}

PDFOperationResult PDFRepairDiffEngine::compare(const PDFDocument& before,
                                                const PDFDocument& after,
                                                const PDFRepairDiffOptions& options,
                                                PDFRepairDiffReport* report)
{
    if (!report)
    {
        return PDFOperationResult(QStringLiteral("Repair diff report is null."));
    }

    *report = PDFRepairDiffReport();
    report->sourceFingerprint = QString::fromLatin1(before.getSourceDataHash().toHex());
    report->candidateFingerprint = QString::fromLatin1(after.getSourceDataHash().toHex());

    const DocumentSnapshot beforeSnapshot = snapshot(before);
    const DocumentSnapshot afterSnapshot = snapshot(after);
    const int commonPageCount = std::min(beforeSnapshot.pageCount, afterSnapshot.pageCount);

    if (beforeSnapshot.pageCount != afterSnapshot.pageCount)
    {
        addChange(report, QStringLiteral("document/page_count"), QStringLiteral("page_count"),
                  QString::number(beforeSnapshot.pageCount), QString::number(afterSnapshot.pageCount), options);
    }
    for (int pageIndex = 0; pageIndex < commonPageCount; ++pageIndex)
    {
        const PageSnapshot& left = beforeSnapshot.pages.at(pageIndex);
        const PageSnapshot& right = afterSnapshot.pages.at(pageIndex);
        const QString prefix = QStringLiteral("pages/%1/").arg(pageIndex);
        // CropBox/BleedBox/TrimBox/ArtBox each fall back to an enclosing box
        // (ultimately MediaBox) whenever a page does not set them explicitly, so a
        // single MediaBox resize cascades into every inherited box reading as changed
        // too. Report that as the one page-box edit it actually is instead of one
        // structural change per inherited box.
        struct NamedBox { const char* name; const QRectF* before; const QRectF* after; };
        const NamedBox namedBoxes[] = {
            { "media_box", &left.mediaBox, &right.mediaBox },
            { "crop_box", &left.cropBox, &right.cropBox },
            { "bleed_box", &left.bleedBox, &right.bleedBox },
            { "trim_box", &left.trimBox, &right.trimBox },
            { "art_box", &left.artBox, &right.artBox },
        };
        QStringList beforeBoxes;
        QStringList afterBoxes;
        for (const NamedBox& namedBox : namedBoxes)
        {
            if (*namedBox.before != *namedBox.after)
            {
                const QString label = QString::fromLatin1(namedBox.name);
                beforeBoxes.append(label + QLatin1Char('=') + rectName(*namedBox.before));
                afterBoxes.append(label + QLatin1Char('=') + rectName(*namedBox.after));
            }
        }
        if (!beforeBoxes.isEmpty())
        {
            addChange(report, prefix + QStringLiteral("page_box"), QStringLiteral("page_box"),
                      beforeBoxes.join(QStringLiteral("; ")), afterBoxes.join(QStringLiteral("; ")), options);
        }
        if (left.rotation != right.rotation) addChange(report, prefix + QStringLiteral("rotation"), QStringLiteral("page_order"), pageRotationName(left.rotation), pageRotationName(right.rotation), options);
        if (options.compareAnnotations && left.annotationTypes != right.annotationTypes)
        {
            addChange(report, prefix + QStringLiteral("annotations"), QStringLiteral("annotation"), left.annotationTypes.join(','), right.annotationTypes.join(','), options);
        }
        if (left.contentDigest != right.contentDigest) addChange(report, prefix + QStringLiteral("content"), QStringLiteral("content"), left.contentDigest, right.contentDigest, options);
        if (options.compareResources)
        {
            if (left.fontsDigest != right.fontsDigest) addChange(report, prefix + QStringLiteral("fonts"), QStringLiteral("font"), left.fontsDigest, right.fontsDigest, options);
            if (left.imagesDigest != right.imagesDigest) addChange(report, prefix + QStringLiteral("images"), QStringLiteral("image"), left.imagesDigest, right.imagesDigest, options);
            if (left.colorSpacesDigest != right.colorSpacesDigest) addChange(report, prefix + QStringLiteral("color_spaces"), QStringLiteral("color_space"), left.colorSpacesDigest, right.colorSpacesDigest, options);
        }
    }

    if (beforeSnapshot.outputIntents != afterSnapshot.outputIntents)
    {
        addChange(report, QStringLiteral("document/output_intents"), QStringLiteral("output_intent"),
                  beforeSnapshot.outputIntents.join(','), afterSnapshot.outputIntents.join(','), options);
    }
    if (options.compareMetadata && beforeSnapshot.metadataDigest != afterSnapshot.metadataDigest)
    {
        addChange(report, QStringLiteral("document/metadata"), QStringLiteral("metadata"), beforeSnapshot.metadataDigest, afterSnapshot.metadataDigest, options);
    }
    if (beforeSnapshot.annotationCount != afterSnapshot.annotationCount)
    {
        addChange(report, QStringLiteral("document/annotation_count"), QStringLiteral("annotation"),
                  QString::number(beforeSnapshot.annotationCount), QString::number(afterSnapshot.annotationCount), options);
    }
    if (beforeSnapshot.hasSignatures != afterSnapshot.hasSignatures)
    {
        addChange(report, QStringLiteral("document/signatures"), QStringLiteral("signature"),
                  beforeSnapshot.hasSignatures ? QStringLiteral("present") : QStringLiteral("absent"),
                  afterSnapshot.hasSignatures ? QStringLiteral("present") : QStringLiteral("absent"), options);
    }
    if (beforeSnapshot.embeddedFileCount != afterSnapshot.embeddedFileCount)
    {
        addChange(report, QStringLiteral("document/embedded_files"), QStringLiteral("embedded_file"),
                  QString::number(beforeSnapshot.embeddedFileCount), QString::number(afterSnapshot.embeddedFileCount), options);
    }

    std::sort(report->structuralChanges.begin(), report->structuralChanges.end(), [](const PDFRepairStructuralChange& left, const PDFRepairStructuralChange& right)
    {
        return std::tie(left.path, left.kind) < std::tie(right.path, right.kind);
    });

    if (options.renderVisualDiff)
    {
        const int pageLimit = std::min({ commonPageCount, options.maxRenderedPages, 200 });
        qint64 consumedPixels = 0;
        for (int pageIndex = 0; pageIndex < pageLimit; ++pageIndex)
        {
            if (PDFOperationControl::isOperationCancelled(options.operationControl))
            {
                report->status = PDFRepairDiffStatus::Incomplete;
                report->incompleteReasons.append(QStringLiteral("cancelled"));
                break;
            }

            const PDFPage* beforePage = before.getCatalog()->getPage(static_cast<size_t>(pageIndex));
            const PDFPage* afterPage = after.getCatalog()->getPage(static_cast<size_t>(pageIndex));
            const QRectF region = commonRegion(beforePage, afterPage);
            const int width = qCeil(region.width() * options.renderDpi / 72.0);
            const int height = qCeil(region.height() * options.renderDpi / 72.0);
            const qint64 pixels = qint64(width) * qint64(height);
            if (region.isEmpty() || width <= 0 || height <= 0)
            {
                report->status = PDFRepairDiffStatus::Incomplete;
                report->incompleteReasons.append(QStringLiteral("page-%1-empty-common-region").arg(pageIndex));
                continue;
            }
            if (pixels > options.maxRenderPixels - consumedPixels)
            {
                report->status = PDFRepairDiffStatus::Incomplete;
                report->incompleteReasons.append(QStringLiteral("render-pixel-budget-exceeded"));
                break;
            }
            consumedPixels += pixels;

            QStringList beforeWarnings;
            QStringList afterWarnings;
            const QImage beforeImage = renderPage(before, pageIndex, region, options.renderDpi, &beforeWarnings);
            const QImage afterImage = renderPage(after, pageIndex, region, options.renderDpi, &afterWarnings);
            if (beforeImage.isNull() || afterImage.isNull() || beforeImage.size() != afterImage.size())
            {
                report->status = PDFRepairDiffStatus::Incomplete;
                report->incompleteReasons.append(QStringLiteral("page-%1-render-failed").arg(pageIndex));
                continue;
            }
            PDFRepairPageVisualDiff pageDiff = compareImages(beforeImage, afterImage, region, options, pageIndex);
            pageDiff.commonRegionCompared = beforePage->getCropBox() == afterPage->getCropBox();
            pageDiff.warnings.append(beforeWarnings);
            pageDiff.warnings.append(afterWarnings);
            if (!pageDiff.commonRegionCompared)
            {
                pageDiff.warnings.append(QStringLiteral("expanded-region-not-rendered"));
            }
            report->pages.append(std::move(pageDiff));
        }
        if (pageLimit < commonPageCount)
        {
            report->status = PDFRepairDiffStatus::Incomplete;
            report->incompleteReasons.append(QStringLiteral("max-rendered-pages-exceeded"));
        }
    }

    if (!report->incompleteReasons.isEmpty())
    {
        report->status = PDFRepairDiffStatus::Incomplete;
    }
    else if (std::any_of(report->pages.cbegin(), report->pages.cend(), [](const PDFRepairPageVisualDiff& page) { return !page.warnings.isEmpty(); }))
    {
        report->status = PDFRepairDiffStatus::CompleteWithWarnings;
    }
    return PDFOperationResult(true);
}

PDFOperationResult PDFRepairDiffEngine::buildSerializedCandidate(
    const PDFDocument& source,
    const std::function<PDFOperationResult(PDFDocument*)>& applyRepair,
    const QString& candidatePath,
    PDFDocument* reopenedCandidate,
    QByteArray* candidateSha256)
{
    if (!reopenedCandidate || candidatePath.isEmpty())
    {
        return PDFOperationResult(QStringLiteral("Repair candidate destination is invalid."));
    }

    PDFDocument candidate = source;
    const PDFOperationResult repairResult = applyRepair(&candidate);
    if (!repairResult)
    {
        return repairResult;
    }

    QDir().mkpath(QFileInfo(candidatePath).absolutePath());
    PDFDocumentWriter writer(nullptr);
    const PDFOperationResult writeResult = writer.write(candidatePath, &candidate, true);
    if (!writeResult)
    {
        return writeResult;
    }

    PDFDocumentReader reader(nullptr, [] (bool*) { return QString(); }, false, false);
    *reopenedCandidate = reader.readFromFile(candidatePath);
    if (reader.getReadingResult() != PDFDocumentReader::Result::OK)
    {
        return PDFOperationResult(QStringLiteral("Serialized repair candidate could not be reopened: %1").arg(reader.getErrorMessage()));
    }
    if (candidateSha256)
    {
        *candidateSha256 = QCryptographicHash::hash(reader.getSource(), QCryptographicHash::Sha256);
    }
    return PDFOperationResult(true);
}

} // namespace pdf
