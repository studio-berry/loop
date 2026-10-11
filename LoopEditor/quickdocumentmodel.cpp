// MIT License
#include "quickdocumentmodel.h"

#include "pdfaction.h"
#include "pdfcatalog.h"
#include "pdfdocument.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsearch.h"
#include "pdfdocumentsession.h"
#include "pdfexception.h"
#include "pdfform.h"
#include "pdfoutline.h"
#include "pdfpage.h"
#include "pdfutils.h"

#include <QRegularExpression>

QuickPageModel::QuickPageModel(QObject* parent) :
    QAbstractListModel(parent)
{
}

int QuickPageModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return m_pages.size();
}

QVariant QuickPageModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_pages.size())
        return {};

    const auto& page = m_pages.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case PageNumberRole:
            return index.row() + 1;
        case WidthRole:
            return page.width;
        case HeightRole:
            return page.height;
        case RotationRole:
            return page.rotation;
        case LabelRole:
            return QString::number(index.row() + 1);
    }
    return {};
}

QHash<int, QByteArray> QuickPageModel::roleNames() const
{
    return { { PageNumberRole, "pageNumber" }, { WidthRole, "pageWidth" }, { HeightRole, "pageHeight" }, { RotationRole, "pageRotation" }, { LabelRole, "label" } };
}

void QuickPageModel::replace(const pdf::PDFDocument* document)
{
    beginResetModel();
    m_pages.clear();
    if (document)
    {
        const pdf::PDFCatalog* catalog = document->getCatalog();
        m_pages.reserve(static_cast<qsizetype>(catalog->getPageCount()));
        for (size_t i = 0; i < catalog->getPageCount(); ++i)
        {
            const pdf::PDFPage* page = catalog->getPage(i);
            m_pages.append(Page{ page->getCropBox().width(), page->getCropBox().height(),
                                 static_cast<int>(page->getPageRotation()), static_cast<int>(i) });
        }
    }
    endResetModel();
}

void QuickPageModel::clear()
{
    replace(nullptr);
}

QuickOutlineModel::QuickOutlineModel(QObject* parent) :
    QAbstractItemModel(parent),
    m_root(std::make_unique<Node>())
{
}
QuickOutlineModel::~QuickOutlineModel() = default;

QuickOutlineModel::Node* QuickOutlineModel::nodeForIndex(const QModelIndex& index) const
{
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : m_root.get();
}

QModelIndex QuickOutlineModel::indexForNode(Node* node) const
{
    if (!node || node == m_root.get() || !node->parent)
        return {};
    for (int row = 0; row < static_cast<int>(node->parent->children.size()); ++row)
    {
        if (node->parent->children.at(row).get() == node)
            return createIndex(row, 0, node);
    }
    return {};
}

QModelIndex QuickOutlineModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column != 0 || row < 0)
        return {};
    Node* parentNode = nodeForIndex(parent);
    if (!parentNode || row >= static_cast<int>(parentNode->children.size()))
        return {};
    return createIndex(row, column, parentNode->children.at(static_cast<size_t>(row)).get());
}

QModelIndex QuickOutlineModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return {};
    return indexForNode(static_cast<Node*>(child.internalPointer())->parent);
}

int QuickOutlineModel::rowCount(const QModelIndex& parent) const
{
    return static_cast<int>(nodeForIndex(parent)->children.size());
}

int QuickOutlineModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant QuickOutlineModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    const Node* node = static_cast<Node*>(index.internalPointer());
    if (!node->item)
        return {};
    if (role == Qt::DisplayRole || role == TitleRole)
        return node->item->getTitle();
    if (role == HasChildrenRole)
        return !node->children.empty();
    if (role == PageRole)
    {
        const pdf::PDFAction* action = node->item->getAction();
        if (!action)
            return -1;
        if (action->getType() == pdf::ActionType::GoTo)
        {
            const auto* goTo = static_cast<const pdf::PDFActionGoTo*>(action);
            const pdf::PDFDestination& dest = goTo->getDestination();
            if (dest.isValid() && !dest.isNamedDestination())
                return static_cast<int>(dest.getPageIndex());
            const pdf::PDFDestination& structDest = goTo->getStructureDestination();
            if (structDest.isValid() && !structDest.isNamedDestination())
                return static_cast<int>(structDest.getPageIndex());
        }
        return -1;
    }
    return {};
}

QHash<int, QByteArray> QuickOutlineModel::roleNames() const
{
    return { { TitleRole, "title" }, { HasChildrenRole, "hasChildren" }, { PageRole, "page" } };
}

void QuickOutlineModel::build(Node* parent, const pdf::PDFOutlineItem* item)
{
    if (!item)
        return;
    for (size_t i = 0; i < item->getChildCount(); ++i)
    {
        auto child = std::make_unique<Node>();
        child->item = item->getChild(i);
        child->parent = parent;
        Node* childNode = child.get();
        parent->children.push_back(std::move(child));
        build(childNode, childNode->item);
    }
}

void QuickOutlineModel::replace(const pdf::PDFDocument* document)
{
    beginResetModel();
    m_root = std::make_unique<Node>();
    if (document)
        build(m_root.get(), document->getCatalog()->getOutlineRootPtr().data());
    endResetModel();
}

void QuickOutlineModel::clear()
{
    replace(nullptr);
}

QuickSearchResultModel::QuickSearchResultModel(QObject* parent) :
    QAbstractListModel(parent)
{
}

int QuickSearchResultModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_results.size();
}

QVariant QuickSearchResultModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size())
        return {};
    const Result& result = m_results.at(index.row());
    if (role == Qt::DisplayRole || role == MatchedRole)
        return result.matched;
    if (role == PageRole)
        return result.page;
    if (role == ContextRole)
        return result.context;
    return {};
}

QHash<int, QByteArray> QuickSearchResultModel::roleNames() const
{
    return { { PageRole, "page" }, { MatchedRole, "matched" }, { ContextRole, "context" } };
}

void QuickSearchResultModel::replace(QList<Result> results, QString query, QString revision)
{
    beginResetModel();
    m_results = std::move(results);
    m_query = std::move(query);
    m_revision = std::move(revision);
    endResetModel();
}

void QuickSearchResultModel::clear()
{
    replace({}, {}, {});
}

namespace
{

/// Stable, operator-facing kind name for an annotation type. A local switch
/// rather than Core's GUI caption: the caption is a display string with
/// historical quirks (it labels Link as Line) and is not a vocabulary.
QString annotationKindName(pdf::AnnotationType type)
{
    switch (type)
    {
        case pdf::AnnotationType::Text:
            return QStringLiteral("text");
        case pdf::AnnotationType::Link:
            return QStringLiteral("link");
        case pdf::AnnotationType::FreeText:
            return QStringLiteral("free-text");
        case pdf::AnnotationType::Line:
            return QStringLiteral("line");
        case pdf::AnnotationType::Square:
            return QStringLiteral("square");
        case pdf::AnnotationType::Circle:
            return QStringLiteral("circle");
        case pdf::AnnotationType::Polygon:
            return QStringLiteral("polygon");
        case pdf::AnnotationType::Polyline:
            return QStringLiteral("polyline");
        case pdf::AnnotationType::Highlight:
            return QStringLiteral("highlight");
        case pdf::AnnotationType::Underline:
            return QStringLiteral("underline");
        case pdf::AnnotationType::Squiggly:
            return QStringLiteral("squiggly");
        case pdf::AnnotationType::StrikeOut:
            return QStringLiteral("strike-out");
        case pdf::AnnotationType::Stamp:
            return QStringLiteral("stamp");
        case pdf::AnnotationType::Caret:
            return QStringLiteral("caret");
        case pdf::AnnotationType::Ink:
            return QStringLiteral("ink");
        case pdf::AnnotationType::Popup:
            return QStringLiteral("popup");
        case pdf::AnnotationType::FileAttachment:
            return QStringLiteral("file-attachment");
        case pdf::AnnotationType::Sound:
            return QStringLiteral("sound");
        case pdf::AnnotationType::Movie:
            return QStringLiteral("movie");
        case pdf::AnnotationType::Widget:
            return QStringLiteral("widget");
        case pdf::AnnotationType::Screen:
            return QStringLiteral("screen");
        case pdf::AnnotationType::PrinterMark:
            return QStringLiteral("printer-mark");
        case pdf::AnnotationType::TrapNet:
            return QStringLiteral("trap-net");
        case pdf::AnnotationType::Watermark:
            return QStringLiteral("watermark");
        case pdf::AnnotationType::Redact:
            return QStringLiteral("redact");
        case pdf::AnnotationType::Projection:
            return QStringLiteral("projection");
        case pdf::AnnotationType::_3D:
            return QStringLiteral("3d");
        case pdf::AnnotationType::RichMedia:
            return QStringLiteral("rich-media");
        case pdf::AnnotationType::Invalid:
        case pdf::AnnotationType::Unknown:
            break;
    }

    return QStringLiteral("unknown");
}

/// Reads the document's annotations, page by page, into review rows. Read-only:
/// Core's own parse of each `/Annots` entry, no annotation manager, no mutation
/// surface. One unparseable annotation is skipped rather than failing the read.
QList<QuickAnnotation> readAnnotations(const pdf::PDFDocument* document)
{
    QList<QuickAnnotation> rows;
    const pdf::PDFCatalog* catalog = document->getCatalog();
    const size_t pageCount = catalog->getPageCount();
    for (size_t pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const pdf::PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        for (const pdf::PDFObjectReference& reference : page->getAnnotations())
        {
            try
            {
                const pdf::PDFAnnotationPtr annotation = pdf::PDFAnnotation::parse(&document->getStorage(), reference);
                if (!annotation || annotation->getType() == pdf::AnnotationType::Popup)
                {
                    // A popup is the open state of a parent annotation, not a
                    // separate review row.
                    continue;
                }

                QuickAnnotation row;
                row.pageIndex = int(pageIndex);
                row.kindName = annotationKindName(annotation->getType());
                row.bounds = annotation->getRectangle();
                row.contents = annotation->getContents();
                if (const pdf::PDFMarkupAnnotation* markup = annotation->asMarkupAnnotation())
                {
                    row.title = markup->getWindowTitle();
                    row.subject = markup->getSubject();
                    row.created = markup->getCreationDate().toString(Qt::ISODate);
                }
                row.reference = QStringLiteral("%1 %2 R").arg(reference.objectNumber).arg(reference.generation);
                const pdf::PDFAnnotation::Flags flags = annotation->getEffectiveFlags();
                row.hidden = flags.testFlag(pdf::PDFAnnotation::Hidden) ||
                             flags.testFlag(pdf::PDFAnnotation::Invisible) ||
                             flags.testFlag(pdf::PDFAnnotation::NoView);
                rows.append(row);
            }
            catch (const pdf::PDFException&)
            {
                continue;
            }
        }
    }
    return rows;
}

}   // namespace

QuickAnnotationModel::QuickAnnotationModel(QObject* parent) :
    QAbstractListModel(parent)
{
}

int QuickAnnotationModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_annotations.size());
}

QVariant QuickAnnotationModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_annotations.size())
        return {};
    const QuickAnnotation& annotation = m_annotations.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case KindRole:
            return annotation.kindName;
        case PageIndexRole:
            return annotation.pageIndex;
        case PageNumberRole:
            return annotation.pageIndex + 1;
        case BoundsRole:
            return annotation.bounds;
        case TitleRole:
            return annotation.title;
        case ContentsRole:
            return annotation.contents;
        case SubjectRole:
            return annotation.subject;
        case CreatedRole:
            return annotation.created;
        case ReferenceRole:
            return annotation.reference;
        case HiddenRole:
            return annotation.hidden;
        case SelectedRole:
            return index.row() == m_selectedRow;
    }
    return {};
}

QHash<int, QByteArray> QuickAnnotationModel::roleNames() const
{
    return { { PageIndexRole, "pageIndex" },
             { PageNumberRole, "pageNumber" },
             { KindRole, "kind" },
             { BoundsRole, "bounds" },
             { TitleRole, "title" },
             { ContentsRole, "contents" },
             { SubjectRole, "subject" },
             { CreatedRole, "created" },
             { ReferenceRole, "reference" },
             { HiddenRole, "hidden" },
             { SelectedRole, "selected" } };
}

void QuickAnnotationModel::replace(QList<QuickAnnotation> annotations, QString revision)
{
    beginResetModel();
    m_annotations = std::move(annotations);
    m_revision = std::move(revision);
    m_selectedRow = -1;
    endResetModel();
}

void QuickAnnotationModel::clear()
{
    replace({}, {});
}

void QuickAnnotationModel::setSelectedRow(int row)
{
    const int bounded = row >= 0 && row < m_annotations.size() ? row : -1;
    if (bounded == m_selectedRow)
    {
        return;
    }
    const int previous = m_selectedRow;
    m_selectedRow = bounded;
    const auto notifyRow = [this](int changed)
    {
        if (changed < 0)
        {
            return;
        }
        const QModelIndex index = this->index(changed, 0);
        Q_EMIT dataChanged(index, index, { SelectedRole });
    };
    notifyRow(previous);
    notifyRow(bounded);
}

QuickDocumentModel::QuickDocumentModel(QObject* parent) :
    QObject(parent),
    m_pages(this),
    m_outline(this),
    m_searchResults(this),
    m_annotations(this)
{
}

void QuickDocumentModel::setDocument(pdf::PDFDocumentContext* context)
{
    m_context = context;
    m_session = context ? context->getSession() : nullptr;
    const pdf::PDFDocument* document = context ? context->getDocument() : nullptr;

    if (!document || !m_session)
    {
        clear();
        return;
    }

    m_pages.replace(document);
    m_outline.replace(document);
    m_annotations.replace(readAnnotations(document), context->getRevision().toString());
    const pdf::PDFDocumentInfo* info = document->getInfo();
    const pdf::PDFCatalog* catalog = document->getCatalog();
    m_title = info->title;
    m_author = info->author;
    m_subject = info->subject;
    m_creator = info->creator;
    m_producer = info->producer;
    m_version = QString::fromLatin1(document->getVersion());
    m_revision = context->getRevision().toString();
    m_hasOutline = catalog->getOutlineRootPtr() && catalog->getOutlineRootPtr()->getChildCount() > 0;
    m_hasAttachments = !catalog->getEmbeddedFiles().empty();
    m_hasOptionalContent = !catalog->getOptionalContentProperties()->getAllOptionalContentGroups().empty();
    m_hasForm = !catalog->getFormObject().isNull();
    m_hasLogicalStructure = catalog->isLogicalStructureMarked();

    const pdf::PDFSecurityHandler* security = document->getStorage().getSecurityHandler();
    m_encrypted = security && security->getMode() != pdf::EncryptionMode::None;
    m_canPrint = security && (security->isAllowed(pdf::PDFSecurityHandler::Permission::PrintLowResolution) ||
                              security->isAllowed(pdf::PDFSecurityHandler::Permission::PrintHighResolution));
    m_canHighResolutionPrint = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::PrintHighResolution);
    m_canCopy = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::CopyContent);
    m_canModify = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::Modify);
    m_canComment = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::ModifyInteractiveItems);
    m_canFillForms = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::ModifyFormFields);
    m_canAssemble = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::Assemble);
    m_canAccessibility = security && security->isAllowed(pdf::PDFSecurityHandler::Permission::Accessibility);
    m_searchResults.clear();
    Q_EMIT changed();
    Q_EMIT searchChanged();
}

void QuickDocumentModel::setLifecycleState(QString state,
                                           bool modified,
                                           bool stale,
                                           QString outputState,
                                           QString typedError)
{
    const bool stateChanged = m_lifecycleState != state || m_modified != modified || m_stale != stale ||
                              m_outputState != outputState || m_typedError != typedError ||
                              m_outputPending != (outputState == QStringLiteral("pending")) ||
                              m_outputSaved != (outputState == QStringLiteral("saved"));
    if (!stateChanged)
    {
        return;
    }

    m_lifecycleState = std::move(state);
    m_modified = modified;
    m_stale = stale;
    m_outputState = std::move(outputState);
    m_typedError = std::move(typedError);
    m_outputPending = m_outputState == QStringLiteral("pending");
    m_outputSaved = m_outputState == QStringLiteral("saved");
    Q_EMIT changed();
}

void QuickDocumentModel::clear()
{
    m_pages.clear();
    m_outline.clear();
    m_searchResults.clear();
    m_annotations.clear();
    m_context = nullptr;
    m_session = nullptr;
    m_title.clear();
    m_author.clear();
    m_subject.clear();
    m_creator.clear();
    m_producer.clear();
    m_version.clear();
    m_revision.clear();
    m_hasOutline = false;
    m_hasAttachments = false;
    m_hasOptionalContent = false;
    m_hasForm = false;
    m_hasLogicalStructure = false;
    m_encrypted = false;
    m_canPrint = false;
    m_canHighResolutionPrint = false;
    m_canCopy = false;
    m_canModify = false;
    m_canComment = false;
    m_canFillForms = false;
    m_canAssemble = false;
    m_canAccessibility = false;
    m_modified = false;
    m_stale = false;
    m_outputPending = false;
    m_outputSaved = false;
    m_lifecycleState.clear();
    m_outputState.clear();
    m_typedError.clear();
    Q_EMIT changed();
    Q_EMIT searchChanged();
}

bool QuickDocumentModel::search(const QString& query)
{
    if (!m_context || !m_session || query.trimmed().isEmpty())
    {
        clearSearch();
        return false;
    }

    const pdf::PDFDocumentSearchResult searchResult = pdf::searchDocumentText(m_context, query);
    if (!searchResult.admitted || !searchResult.completed)
    {
        clearSearch();
        return false;
    }

    QList<QuickSearchResultModel::Result> results;
    results.reserve(searchResult.matches.size());
    for (const pdf::PDFDocumentSearchMatch& match : searchResult.matches)
        results.append({ static_cast<int>(match.pageIndex), match.matched, match.context });
    m_searchResults.replace(std::move(results), query, searchResult.revision.toString());
    Q_EMIT searchChanged();
    return true;
}

void QuickDocumentModel::clearSearch()
{
    m_searchResults.clear();
    Q_EMIT searchChanged();
}

int QuickDocumentModel::searchPageAt(int row) const
{
    const QModelIndex index = m_searchResults.index(row, 0);
    return index.isValid() ? m_searchResults.data(index, QuickSearchResultModel::PageRole).toInt() : -1;
}
