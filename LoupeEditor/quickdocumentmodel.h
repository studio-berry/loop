// MIT License
#ifndef QUICKDOCUMENTMODEL_H
#define QUICKDOCUMENTMODEL_H

#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

#include <memory>
#include <vector>

namespace pdf
{
class PDFDocumentContext;
class PDFDocumentSession;
class PDFOutlineItem;
class PDFDocument;
}

class QuickPageModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        PageNumberRole = Qt::UserRole + 1,
        WidthRole,
        HeightRole,
        RotationRole,
        LabelRole,
    };

    explicit QuickPageModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(const pdf::PDFDocument* document);
    void clear();

private:
    struct Page
    {
        qreal width = 0.0;
        qreal height = 0.0;
        int rotation = 0;
        int index = -1;
    };

    QList<Page> m_pages;
};

class QuickOutlineModel final : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Role
    {
        TitleRole = Qt::UserRole + 1,
        HasChildrenRole,
    };

    explicit QuickOutlineModel(QObject* parent = nullptr);
    ~QuickOutlineModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(const pdf::PDFDocument* document);
    void clear();

private:
    struct Node
    {
        const pdf::PDFOutlineItem* item = nullptr;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

    void build(Node* parent, const pdf::PDFOutlineItem* item);
    Node* nodeForIndex(const QModelIndex& index) const;
    QModelIndex indexForNode(Node* node) const;

    std::unique_ptr<Node> m_root;
};

class QuickSearchResultModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        PageRole = Qt::UserRole + 1,
        MatchedRole,
        ContextRole,
    };

    struct Result
    {
        int page = -1;
        QString matched;
        QString context;
    };

    explicit QuickSearchResultModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(QList<Result> results, QString query, QString revision);
    void clear();
    QString query() const noexcept { return m_query; }
    QString revision() const noexcept { return m_revision; }

private:
    QList<Result> m_results;
    QString m_query;
    QString m_revision;
};

class QuickDocumentModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* pages READ pages CONSTANT)
    Q_PROPERTY(QAbstractItemModel* outline READ outline CONSTANT)
    Q_PROPERTY(QAbstractItemModel* searchResults READ searchResults CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString author READ author NOTIFY changed)
    Q_PROPERTY(QString subject READ subject NOTIFY changed)
    Q_PROPERTY(QString creator READ creator NOTIFY changed)
    Q_PROPERTY(QString producer READ producer NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString revision READ revision NOTIFY changed)
    Q_PROPERTY(bool hasOutline READ hasOutline NOTIFY changed)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY changed)
    Q_PROPERTY(bool hasOptionalContent READ hasOptionalContent NOTIFY changed)
    Q_PROPERTY(int searchResultCount READ searchResultCount NOTIFY searchChanged)

public:
    explicit QuickDocumentModel(QObject* parent = nullptr);

    QAbstractItemModel* pages() noexcept { return &m_pages; }
    QAbstractItemModel* outline() noexcept { return &m_outline; }
    QAbstractItemModel* searchResults() noexcept { return &m_searchResults; }

    QString title() const { return m_title; }
    QString author() const { return m_author; }
    QString subject() const { return m_subject; }
    QString creator() const { return m_creator; }
    QString producer() const { return m_producer; }
    QString version() const { return m_version; }
    QString revision() const { return m_revision; }
    bool hasOutline() const noexcept { return m_hasOutline; }
    bool hasAttachments() const noexcept { return m_hasAttachments; }
    bool hasOptionalContent() const noexcept { return m_hasOptionalContent; }
    int searchResultCount() const noexcept { return m_searchResults.rowCount(); }

    void setDocument(pdf::PDFDocumentContext* context);
    void clear();

    /// Performs a Core text search against a captured document revision. The
    /// result is admitted only if the context still owns that revision.
    Q_INVOKABLE bool search(const QString& query);
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE int searchPageAt(int row) const;

signals:
    void changed();
    void searchChanged();

private:
    QuickPageModel m_pages;
    QuickOutlineModel m_outline;
    QuickSearchResultModel m_searchResults;
    pdf::PDFDocumentContext* m_context = nullptr;
    pdf::PDFDocumentSession* m_session = nullptr;
    QString m_title;
    QString m_author;
    QString m_subject;
    QString m_creator;
    QString m_producer;
    QString m_version;
    QString m_revision;
    bool m_hasOutline = false;
    bool m_hasAttachments = false;
    bool m_hasOptionalContent = false;
};

#endif
