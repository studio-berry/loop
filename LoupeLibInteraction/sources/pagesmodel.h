#ifndef PAGESMODEL_H
#define PAGESMODEL_H

#include <QAbstractListModel>
#include <QSizeF>
#include <QVector>

namespace pdfinteraction
{

struct PageView
{
    QString id;
    QString label;
    QString source;
    int pageNumber = 0;
    QSizeF sizePt;
    int rotation = 0;
    bool modified = false;
    bool selected = false;
};

class PagesModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString documentKey READ documentKey NOTIFY revisionChanged)
    Q_PROPERTY(QString documentRevision READ documentRevision NOTIFY revisionChanged)
    Q_PROPERTY(QString selectedPageId READ selectedPageId NOTIFY selectionChanged)

public:
    enum Role
    {
        PageIdRole = Qt::UserRole + 1,
        LabelRole,
        SourceRole,
        PageNumberRole,
        SizeRole,
        RotationRole,
        ModifiedRole,
        SelectedRole
    };
    Q_ENUM(Role)

    explicit PagesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool replace(QString documentKey, QString documentRevision, QVector<PageView> pages);
    void setCurrentRevision(QString documentKey, QString documentRevision);
    bool setSelectedPage(const QString& pageId,
                         const QString& documentKey,
                         const QString& documentRevision);
    void clear();

    bool containsCurrent(const QString& pageId,
                         const QString& documentKey,
                         const QString& documentRevision) const;
    const PageView* page(const QString& pageId) const;
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }
    QString selectedPageId() const { return m_selectedPageId; }

signals:
    void revisionChanged();
    void selectionChanged();

private:
    QVector<PageView> m_pages;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_selectedPageId;
};

} // namespace pdfinteraction

#endif // PAGESMODEL_H
