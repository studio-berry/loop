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

#include "loupecanvasaccessible.h"

#include "loupecanvasitem.h"

#include <QAccessible>
#include <QAccessibleObject>
#include <QQuickItem>
#include <QQuickWindow>

namespace pdfquick
{

namespace
{

class LoupeCanvasAccessible final : public QAccessibleObject
{
public:
    explicit LoupeCanvasAccessible(LoupeCanvasItem* item) :
        QAccessibleObject(item),
        m_item(item)
    {
    }

    QRect rect() const override
    {
        if (!m_item || !m_item->window())
        {
            return {};
        }

        const QPoint topLeft = m_item->mapToScene(QPointF(0.0, 0.0)).toPoint();
        return QRect(topLeft, m_item->size().toSize());
    }

    QAccessible::Role role() const override { return QAccessible::Canvas; }

    QAccessible::State state() const override
    {
        QAccessible::State result;
        if (!m_item)
        {
            return result;
        }

        if (m_item->hasActiveFocus())
        {
            result.active = true;
            result.focused = true;
        }
        if (!m_item->isEnabled())
        {
            result.disabled = true;
        }
        return result;
    }

    QString text(QAccessible::Text type) const override
    {
        if (!m_item)
        {
            return {};
        }

        if (type == QAccessible::Name)
        {
            return QStringLiteral("Document canvas");
        }
        if (type == QAccessible::Description)
        {
            return m_item->accessibleDocumentSummary();
        }
        return {};
    }

    int childCount() const override { return 0; }

    QAccessibleInterface* child(int) const override { return nullptr; }

    int indexOfChild(const QAccessibleInterface*) const override { return -1; }

    QAccessibleInterface* parent() const override
    {
        if (!m_item)
        {
            return nullptr;
        }

        if (QQuickItem* parentItem = m_item->parentItem())
        {
            return QAccessible::queryAccessibleInterface(parentItem);
        }

        return nullptr;
    }

private:
    LoupeCanvasItem* m_item = nullptr;
};

QAccessibleInterface* loupeCanvasAccessibleFactory(const QString& classname, QObject* object)
{
    Q_UNUSED(classname);
    if (LoupeCanvasItem* item = qobject_cast<LoupeCanvasItem*>(object))
    {
        return new LoupeCanvasAccessible(item);
    }
    return nullptr;
}

struct AccessibilityRegistrar
{
    AccessibilityRegistrar() { QAccessible::installFactory(&loupeCanvasAccessibleFactory); }
};

}   // namespace

void installLoupeCanvasAccessibility()
{
    static AccessibilityRegistrar registrar;
    Q_UNUSED(registrar);
}

}   // namespace pdfquick
