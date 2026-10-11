// MIT License
// Copyright (c) 2026 Loop contributors

#include "editorsettings.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace
{

const QString RenderOptionsGroup = QStringLiteral("renderOptions");

/// Strict parse of one persisted value. QSettings writes booleans as
/// "true"/"false" (or a boolean in the native backend); anything else is an
/// invalid stored value, not a value to coerce.
bool parseStoredBool(const QVariant& stored, bool* parsed)
{
    if (stored.metaType().id() == QMetaType::Bool)
    {
        *parsed = true;
        return stored.toBool();
    }
    if (stored.metaType().id() == QMetaType::QString)
    {
        const QString text = stored.toString().trimmed();
        if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        {
            *parsed = true;
            return true;
        }
        if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
        {
            *parsed = true;
            return false;
        }
    }

    *parsed = false;
    return false;
}

}   // namespace

EditorSettings::EditorSettings(QSettings* settings, QObject* parent) :
    QObject(parent),
    m_store(settings)
{
    m_options = {
        { AntialiasingOptionId,
          tr("Antialiasing"),
          tr("Smooth the edges of lines, shapes and curves on the page."),
          RenderOptionsGroup + QStringLiteral("/antialiasing"),
          true,
          true },
        { TextAntialiasingOptionId,
          tr("Text antialiasing"),
          tr("Smooth the rendering of text on the page."),
          RenderOptionsGroup + QStringLiteral("/textAntialiasing"),
          true,
          true },
        { SmoothPicturesOptionId,
          tr("Smooth pictures"),
          tr("Scale images with smooth interpolation. Slower, but better image quality."),
          RenderOptionsGroup + QStringLiteral("/smoothPictures"),
          false,
          false },
        { DisplayAnnotationsOptionId,
          tr("Display annotations"),
          tr("Draw the document's annotations over the page."),
          RenderOptionsGroup + QStringLiteral("/displayAnnotations"),
          true,
          true },
        { IgnoreOptionalContentOptionId,
          tr("Ignore optional content settings"),
          tr("Show all optional content regardless of the document's own layer settings. The "
             "preview no longer matches the document's view state; the fidelity indicator "
             "reports the deviation."),
          RenderOptionsGroup + QStringLiteral("/ignoreOptionalContentSettings"),
          false,
          false },
        { DisplayRenderTimesOptionId,
          tr("Display render times"),
          tr("Draw the page compile and draw time over the page."),
          RenderOptionsGroup + QStringLiteral("/displayRenderTimes"),
          false,
          false },
        { ShowTextBlocksOptionId,
          tr("Show text blocks"),
          tr("Draw the text block layout the renderer parsed."),
          RenderOptionsGroup + QStringLiteral("/showTextBlocks"),
          false,
          false },
        { ShowTextLinesOptionId,
          tr("Show text lines"),
          tr("Draw the text line layout the renderer parsed."),
          RenderOptionsGroup + QStringLiteral("/showTextLines"),
          false,
          false },
    };
}

EditorSettings::~EditorSettings() = default;

QSettings& EditorSettings::store()
{
    if (!m_store)
    {
        // The owned fallback: the same IniFormat/UserScope idiom Core uses for
        // the diagnostics settings, so a host constructed without an explicit
        // store still persists under the application identity.
        m_ownedStore = std::make_unique<QSettings>(QSettings::IniFormat,
                                                   QSettings::UserScope,
                                                   QCoreApplication::organizationName(),
                                                   QCoreApplication::applicationName());
        m_store = m_ownedStore.get();
    }
    return *m_store;
}

void EditorSettings::load()
{
    if (m_loaded)
    {
        return;
    }
    m_loaded = true;

    QSettings& settings = store();
    for (Option& option : m_options)
    {
        if (!settings.contains(option.settingsKey))
        {
            option.value = option.defaultValue;
            continue;
        }

        bool parsed = false;
        const bool value = parseStoredBool(settings.value(option.settingsKey), &parsed);
        if (parsed)
        {
            option.value = value;
            continue;
        }

        const QString stored = settings.value(option.settingsKey).toString();
        option.value = option.defaultValue;
        m_loadNotices.append(tr("\"%1\" was stored as \"%2\", which is not a boolean; it was replaced by its "
                                "default (%3).")
                                 .arg(option.settingsKey,
                                      stored,
                                      option.defaultValue ? tr("enabled") : tr("disabled")));
        if (!writeOptionValue(option, option.defaultValue))
        {
            m_loadNotices.append(tr("\"%1\" could not be repaired in the settings store.").arg(option.settingsKey));
        }
    }

    if (!m_loadNotices.isEmpty())
    {
        Q_EMIT noticeChanged();
    }
}

EditorSettings::Option* EditorSettings::findOption(const QString& id)
{
    const auto it = std::find_if(m_options.begin(), m_options.end(), [&id](const Option& option)
                                 { return option.id == id; });
    return it == m_options.end() ? nullptr : &*it;
}

bool EditorSettings::writeOptionValue(const Option& option, bool enabled)
{
    QSettings& settings = store();
    settings.setValue(option.settingsKey, enabled);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void EditorSettings::setOperationNotice(QString text)
{
    if (m_operationNotice == text)
    {
        return;
    }
    m_operationNotice = std::move(text);
    Q_EMIT noticeChanged();
}

EditorSettings::RenderOptions EditorSettings::renderOptions()
{
    load();

    RenderOptions options;
    for (const Option& option : m_options)
    {
        if (option.id == AntialiasingOptionId)
        {
            options.antialiasing = option.value;
        }
        else if (option.id == TextAntialiasingOptionId)
        {
            options.textAntialiasing = option.value;
        }
        else if (option.id == SmoothPicturesOptionId)
        {
            options.smoothPictures = option.value;
        }
        else if (option.id == DisplayAnnotationsOptionId)
        {
            options.displayAnnotations = option.value;
        }
        else if (option.id == IgnoreOptionalContentOptionId)
        {
            options.ignoreOptionalContentSettings = option.value;
        }
        else if (option.id == DisplayRenderTimesOptionId)
        {
            options.displayRenderTimes = option.value;
        }
        else if (option.id == ShowTextBlocksOptionId)
        {
            options.showTextBlocks = option.value;
        }
        else if (option.id == ShowTextLinesOptionId)
        {
            options.showTextLines = option.value;
        }
    }
    return options;
}

bool EditorSettings::isOptionEnabled(const QString& id)
{
    load();

    const Option* option = findOption(id);
    return option && option->value;
}

QVariantList EditorSettings::options()
{
    load();

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(m_options.size()));
    for (const Option& option : m_options)
    {
        QVariantMap row;
        row.insert(QStringLiteral("id"), option.id);
        row.insert(QStringLiteral("label"), option.label);
        row.insert(QStringLiteral("description"), option.description);
        row.insert(QStringLiteral("enabled"), option.value);
        rows.append(row);
    }
    return rows;
}

QString EditorSettings::notice()
{
    load();

    QStringList notices = m_loadNotices;
    if (!m_operationNotice.isEmpty())
    {
        notices.append(m_operationNotice);
    }
    return notices.join(QLatin1Char('\n'));
}

bool EditorSettings::hasNotice()
{
    return !notice().isEmpty();
}

bool EditorSettings::setOptionEnabled(const QString& id, bool enabled)
{
    load();

    Option* option = findOption(id);
    if (!option)
    {
        setOperationNotice(tr("Unknown setting \"%1\" was refused.").arg(id));
        return false;
    }

    if (option->value == enabled)
    {
        return true;
    }

    if (!writeOptionValue(*option, enabled))
    {
        setOperationNotice(tr("\"%1\" could not be saved; the previous value is unchanged.").arg(option->label));
        return false;
    }

    option->value = enabled;
    setOperationNotice(QString());
    Q_EMIT optionsChanged();
    return true;
}

bool EditorSettings::resetToFactory()
{
    load();

    QSettings& settings = store();
    settings.remove(RenderOptionsGroup);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        setOperationNotice(tr("The shell settings could not be reset; the stored values are unchanged."));
        return false;
    }

    bool changed = false;
    for (Option& option : m_options)
    {
        if (option.value != option.defaultValue)
        {
            option.value = option.defaultValue;
            changed = true;
        }
    }

    setOperationNotice(tr("Shell settings were reset to factory defaults."));
    if (changed)
    {
        Q_EMIT optionsChanged();
    }
    return true;
}
