// MIT License
// Copyright (c) 2026 Loop contributors

#ifndef EDITORSETTINGS_H
#define EDITORSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>
#include <vector>

/// The one typed settings store for the Quick shell (#239).
///
/// Owns the persisted render options and diagnostic-view toggles the shell
/// applies to the interactive render path, and the factory reset that returns
/// them to their defaults. Persisted values are parsed once, when the store is
/// first read, into typed state: a stored value that is not a boolean is
/// reported through notice() and replaced by its default, never silently
/// coerced. Every option is presentation state only - no option changes a
/// document.
class EditorSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList options READ options NOTIFY optionsChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(bool hasNotice READ hasNotice NOTIFY noticeChanged)

public:
    /// The typed render options, mirroring pdf::PDFRenderer::Features. The
    /// defaults reproduce PDFRenderer::getDefaultFeatures() plus the off-by-
    /// default diagnostic views.
    struct RenderOptions
    {
        bool antialiasing = true;
        bool textAntialiasing = true;
        bool smoothPictures = false;
        bool displayAnnotations = true;
        bool ignoreOptionalContentSettings = false;
        bool displayRenderTimes = false;
        bool showTextBlocks = false;
        bool showTextLines = false;
    };

    /// The option ids the store persists; the settings surface and the command
    /// handlers name the same ids.
    static inline const QString AntialiasingOptionId = QStringLiteral("antialiasing");
    static inline const QString TextAntialiasingOptionId = QStringLiteral("textAntialiasing");
    static inline const QString SmoothPicturesOptionId = QStringLiteral("smoothPictures");
    static inline const QString DisplayAnnotationsOptionId = QStringLiteral("displayAnnotations");
    static inline const QString IgnoreOptionalContentOptionId = QStringLiteral("ignoreOptionalContentSettings");
    static inline const QString DisplayRenderTimesOptionId = QStringLiteral("displayRenderTimes");
    static inline const QString ShowTextBlocksOptionId = QStringLiteral("showTextBlocks");
    static inline const QString ShowTextLinesOptionId = QStringLiteral("showTextLines");

    /// The backing store stays with the caller and must outlive this object. A
    /// null pointer opens the application's own user-scope settings on first
    /// use, which keeps construction free of I/O for a caller that never reads.
    explicit EditorSettings(QSettings* settings = nullptr, QObject* parent = nullptr);
    ~EditorSettings() override;

    EditorSettings(const EditorSettings&) = delete;
    EditorSettings& operator=(const EditorSettings&) = delete;

    /// The typed snapshot, loading the store on first read.
    RenderOptions renderOptions();

    /// True when `id` is enabled in the typed state. An unknown id answers
    /// false; it is never defaulted to a value.
    bool isOptionEnabled(const QString& id);

    /// Presentation rows for QML: id, label, description, enabled.
    QVariantList options();

    /// Every visible notice: a persisted value that was rejected and replaced,
    /// or the last refused operation. Empty when nothing needs reporting.
    QString notice();
    bool hasNotice();

    /// Persists one option. An unknown id is refused. A failed write leaves
    /// both the typed state and the store unchanged and is reported.
    Q_INVOKABLE bool setOptionEnabled(const QString& id, bool enabled);

    /// Removes every key this store owns and returns the typed state to its
    /// defaults. A failed write is reported and leaves the previous values in
    /// place; the caller confirms before this runs.
    Q_INVOKABLE bool resetToFactory();

signals:
    void optionsChanged();
    void noticeChanged();

private:
    struct Option
    {
        QString id;
        QString label;
        QString description;
        QString settingsKey;
        bool defaultValue = true;
        bool value = true;
    };

    QSettings& store();
    void load();
    Option* findOption(const QString& id);
    bool writeOptionValue(const Option& option, bool enabled);
    void setOperationNotice(QString text);

    std::unique_ptr<QSettings> m_ownedStore;
    QSettings* m_store = nullptr;
    std::vector<Option> m_options;
    QStringList m_loadNotices;
    QString m_operationNotice;
    bool m_loaded = false;
};

#endif   // EDITORSETTINGS_H
