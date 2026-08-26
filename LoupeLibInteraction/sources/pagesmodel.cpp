#include "pagesmodel.h"

#include <QSet>
#include <QVariant>

#include <utility>

namespace pdfinteraction
{

PagesModel::PagesModel(QObject* parent) :
    QAbstractListModel(parent)
{
}

int PagesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_pages.size();
}

QVariant PagesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_pages.size())
    {
        return {};
    }

    const PageView& page = m_pages.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case LabelRole:
            return page.label;
        case PageIdRole:
            return page.id;
        case SourceRole:
            return page.source;
        case PageNumberRole:
            return page.pageNumber;
        case SizeRole:
            return page.sizePt;
        case RotationRole:
            return page.rotation;
        case ModifiedRole:
            return page.modified;
        case SelectedRole:
            return page.selected;
        default:
            return {};
    }
}

QHash<int, QByteArray> PagesModel::roleNames() const
{
    return {
        { PageIdRole, "pageId" },
        { LabelRole, "label" },
        { SourceRole, "source" },
        { PageNumberRole, "pageNumber" },
        { SizeRole, "size" },
        { RotationRole, "rotation" },
        { ModifiedRole, "modified" },
        { SelectedRole, "selected" }
    };
}

bool PagesModel::replace(QString documentKey, QString documentRevision, QVector<PageView> pages)
{
    if ((documentKey.isEmpty() || documentRevision.isEmpty()) && !pages.isEmpty())
    {
        return false;
    }

    QSet<QString> ids;
    for (PageView& page : pages)
    {
        if (page.id.isEmpty() || page.pageNumber <= 0 || ids.contains(page.id))
        {
            return false;
        }
        ids.insert(page.id);
        page.selected = false;
    }

    const QString previousSelection = m_selectedPageId;
    beginResetModel();
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_pages = std::move(pages);
    m_selectedPageId.clear();
    endResetModel();
    if (!previousSelection.isEmpty())
    {
        Q_EMIT selectionChanged();
    }
    Q_EMIT revisionChanged();
    return true;
}

void PagesModel::setCurrentRevision(QString documentKey, QString documentRevision)
{
    if (documentKey == m_documentKey && documentRevision == m_documentRevision)
    {
        return;
    }

    beginResetModel();
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_pages.clear();
    m_selectedPageId.clear();
    endResetModel();
    Q_EMIT revisionChanged();
    Q_EMIT selectionChanged();
}

bool PagesModel::setSelectedPage(const QString& pageId,
                                 const QString& documentKey,
                                 const QString& documentRevision)
{
    if (!containsCurrent(pageId, documentKey, documentRevision))
    {
        return false;
    }

    const QString previous = m_selectedPageId;
    m_selectedPageId = pageId;
    for (int row = 0; row < m_pages.size(); ++row)
    {
        const bool selected = m_pages.at(row).id == pageId;
        if (m_pages[row].selected != selected)
        {
            m_pages[row].selected = selected;
            Q_EMIT dataChanged(index(row), index(row), { SelectedRole });
        }
    }
    if (previous != m_selectedPageId)
    {
        Q_EMIT selectionChanged();
    }
    return true;
}

void PagesModel::clear()
{
    if (m_pages.isEmpty() && m_documentKey.isEmpty() && m_documentRevision.isEmpty())
    {
        return;
    }

    beginResetModel();
    m_pages.clear();
    m_documentKey.clear();
    m_documentRevision.clear();
    m_selectedPageId.clear();
    endResetModel();
    Q_EMIT revisionChanged();
    Q_EMIT selectionChanged();
}

bool PagesModel::containsCurrent(const QString& pageId,
                                 const QString& documentKey,
                                 const QString& documentRevision) const
{
    return documentKey == m_documentKey && documentRevision == m_documentRevision && page(pageId) != nullptr;
}


const PageView* PagesModel::page(const QString& pageId) const
{
    for (const PageView& item : m_pages)
    {
        if (item.id == pageId)
        {
            return &item;
        }
    }
    return nullptr;
}

}   // namespace pdfinteraction
