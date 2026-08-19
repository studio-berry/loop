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

#include "pdfaccessibility.h"

#include "pdfdrawwidget.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QAction>
#include <QComboBox>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <QList>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>

namespace pdf
{

namespace
{

QString cleanMnemonicText(QString text)
{
    text.replace(QStringLiteral("&&"), QStringLiteral("&"));
    text.remove(QChar('&'));
    return text.trimmed();
}

std::optional<QChar> mnemonicFromText(const QString& text)
{
    for (qsizetype i = 0; i < text.size(); ++i)
    {
        if (text.at(i) != QChar('&'))
        {
            continue;
        }
        if (i + 1 < text.size() && text.at(i + 1) == QChar('&'))
        {
            ++i;
            continue;
        }
        if (i + 1 < text.size())
        {
            return text.at(i + 1).toUpper();
        }
    }
    return std::nullopt;
}

QString widgetPath(const QWidget* widget)
{
    QStringList parts;
    for (const QWidget* current = widget; current; current = current->parentWidget())
    {
        const QString name = current->objectName().isEmpty()
                                 ? QString::fromLatin1(current->metaObject()->className())
                                 : current->objectName();
        parts.prepend(name);
    }
    return parts.join(QStringLiteral("/"));
}

bool exempt(const QWidget* widget)
{
    return widget->property("accessibilityExempt").toBool();
}

void appendMnemonicProblems(const QMenu* menu, QVector<MnemonicProblem>& problems)
{
    if (!menu)
    {
        return;
    }

    const QString menuTitle = menu->title();
    if (!menuTitle.isEmpty() && !mnemonicFromText(menuTitle).has_value()
        && !menu->property("accessibilityMnemonicExempt").toBool())
    {
        problems.push_back({MnemonicProblem::Kind::Missing, menuTitle, menuTitle, {}});
    }

    std::map<QChar, QList<QAction*>> actionsByMnemonic;
    for (QAction* action : menu->actions())
    {
        if (action->isSeparator() || action->text().isEmpty()
            || action->property("accessibilityMnemonicExempt").toBool())
        {
            continue;
        }

        if (QMenu* submenu = action->menu())
        {
            appendMnemonicProblems(submenu, problems);
        }

        const std::optional<QChar> mnemonic = mnemonicFromText(action->text());
        if (!mnemonic.has_value())
        {
            problems.push_back({MnemonicProblem::Kind::Missing, menuTitle, action->text(), {}});
            continue;
        }
        actionsByMnemonic[*mnemonic].push_back(action);
    }

    for (const auto& [mnemonic, actions] : actionsByMnemonic)
    {
        if (actions.size() < 2)
        {
            continue;
        }
        for (QAction* action : actions)
        {
            problems.push_back({MnemonicProblem::Kind::Duplicate, menuTitle, action->text(), mnemonic});
        }
    }
}

class PDFDrawWidgetAccessible final : public QAccessibleWidget
{
public:
    explicit PDFDrawWidgetAccessible(PDFDrawWidget* widget) :
        QAccessibleWidget(widget, QAccessible::Canvas)
    {
    }

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Name)
        {
            return QStringLiteral("Document canvas");
        }
        if (type == QAccessible::Description)
        {
            if (const auto* widget = qobject_cast<const PDFDrawWidget*>(object()))
            {
                return widget->accessibleDocumentSummary();
            }
            return QStringLiteral("Inspect the active document page.");
        }
        return QAccessibleWidget::text(type);
    }
};

} // namespace

QVector<AccessibilityFinding> PDFAccessibility::auditWidgetTree(QWidget* root)
{
    QVector<AccessibilityFinding> findings;
    if (!root)
    {
        return findings;
    }

    QList<QWidget*> widgets;
    widgets.push_back(root);
    widgets.append(root->findChildren<QWidget*>());

    for (QWidget* widget : widgets)
    {
        if (exempt(widget) || !widget->isEnabled())
        {
            continue;
        }

        const bool customCanvas = qobject_cast<PDFDrawWidget*>(widget) != nullptr;
        const bool requiresExplicitName = customCanvas
                                           || qobject_cast<QAbstractSpinBox*>(widget) != nullptr
                                           || qobject_cast<QComboBox*>(widget) != nullptr
                                           || qobject_cast<QLineEdit*>(widget) != nullptr
                                           || qobject_cast<QProgressBar*>(widget) != nullptr
                                           || qobject_cast<QAbstractItemView*>(widget) != nullptr;
        const QAbstractButton* button = qobject_cast<QAbstractButton*>(widget);
        const QToolButton* toolButton = qobject_cast<QToolButton*>(widget);
        const bool iconOnlyButton = button && button->text().isEmpty()
                                    && (!toolButton || !toolButton->defaultAction()
                                        || toolButton->defaultAction()->text().isEmpty());

        if (requiresExplicitName && widget->accessibleName().trimmed().isEmpty())
        {
            findings.push_back({widgetPath(widget), QStringLiteral("missing-accessible-name"),
                                QStringLiteral("Interactive control needs an explicit accessible name.")});
        }
        else if (iconOnlyButton && widget->toolTip().trimmed().isEmpty()
                 && widget->accessibleName().trimmed().isEmpty())
        {
            findings.push_back({widgetPath(widget), QStringLiteral("missing-icon-button-name"),
                                QStringLiteral("Icon-only control needs an accessible name or description.")});
        }

        if (requiresExplicitName && !widget->accessibleName().trimmed().isEmpty()
            && (customCanvas || qobject_cast<QProgressBar*>(widget) != nullptr)
            && widget->accessibleDescription().trimmed().isEmpty())
        {
            findings.push_back({widgetPath(widget), QStringLiteral("missing-accessible-description"),
                                QStringLiteral("This control needs a concise description of its state or purpose.")});
        }
    }

    return findings;
}

QVector<MnemonicProblem> PDFAccessibility::auditMenu(const QMenu* menu)
{
    QVector<MnemonicProblem> problems;
    appendMnemonicProblems(menu, problems);
    return problems;
}

QVector<MnemonicProblem> PDFAccessibility::auditMenus(const QMainWindow* window)
{
    if (!window)
    {
        return {};
    }

    QVector<MnemonicProblem> problems;
    if (const QMenuBar* menuBar = window->menuBar())
    {
        for (QAction* action : menuBar->actions())
        {
            if (QMenu* menu = action->menu())
            {
                appendMnemonicProblems(menu, problems);
            }
        }
    }
    return problems;
}

double PDFAccessibility::contrastRatio(const QColor& foreground, const QColor& background)
{
    auto relativeLuminance = [](const QColor& color) {
        auto linear = [](double channel) {
            channel /= 255.0;
            return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.red()) + 0.7152 * linear(color.green()) + 0.0722 * linear(color.blue());
    };

    const double foregroundLuminance = relativeLuminance(foreground);
    const double backgroundLuminance = relativeLuminance(background);
    const double lighter = std::max(foregroundLuminance, backgroundLuminance);
    const double darker = std::min(foregroundLuminance, backgroundLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

bool PDFAccessibility::meetsContrast(const QColor& foreground, const QColor& background, double requiredRatio)
{
    return contrastRatio(foreground, background) >= requiredRatio;
}

void PDFAccessibility::applyActionAccessibility(QToolButton* button, const QAction* action)
{
    if (!button || !action)
    {
        return;
    }

    const QString name = cleanMnemonicText(action->text());
    if (button->accessibleName().isEmpty() && !name.isEmpty())
    {
        button->setAccessibleName(name);
    }
    if (button->accessibleDescription().isEmpty() && !action->toolTip().isEmpty())
    {
        button->setAccessibleDescription(action->toolTip());
    }
}

int PDFAccessibility::minimumSpinBoxWidth(const QAbstractSpinBox* spinBox, const QString& sampleText)
{
    if (!spinBox)
    {
        return 0;
    }

    const QStyle* style = spinBox->style();
    const int frame = style->pixelMetric(QStyle::PM_SpinBoxFrameWidth, nullptr, spinBox);
    const int controls = style->pixelMetric(QStyle::PM_SpinBoxSliderHeight, nullptr, spinBox);
    return spinBox->fontMetrics().horizontalAdvance(sampleText) + (2 * frame) + controls + 4;
}

void PDFAccessibility::install()
{
    static std::once_flag installed;
    std::call_once(installed, [] {
        QAccessible::installFactory([](const QString&, QObject* object) -> QAccessibleInterface* {
            if (PDFDrawWidget* widget = qobject_cast<PDFDrawWidget*>(object))
            {
                return new PDFDrawWidgetAccessible(widget);
            }
            return nullptr;
        });
    });
}

} // namespace pdf
