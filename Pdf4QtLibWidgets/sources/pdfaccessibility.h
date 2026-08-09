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

#ifndef PDFACCESSIBILITY_H
#define PDFACCESSIBILITY_H

#include "pdfwidgetsglobal.h"

#include <QColor>
#include <QVector>
#include <QString>

class QAbstractSpinBox;
class QAction;
class QMainWindow;
class QMenu;
class QMenuBar;
class QToolButton;
class QWidget;

namespace pdf
{

enum class AccessibilityRequirement
{
    InferredTextIsEnough,
    ExplicitName,
    ExplicitNameAndDescription,
    CustomInterface
};

struct PDF4QTLIBWIDGETSSHARED_EXPORT AccessibilityFinding
{
    QString objectPath;
    QString code;
    QString message;
};

struct PDF4QTLIBWIDGETSSHARED_EXPORT MnemonicProblem
{
    enum class Kind
    {
        Missing,
        Duplicate
    };

    Kind kind = Kind::Missing;
    QString menuTitle;
    QString actionText;
    QChar mnemonic;
};

class PDF4QTLIBWIDGETSSHARED_EXPORT PDFAccessibility
{
public:
    PDFAccessibility() = delete;

    static QVector<AccessibilityFinding> auditWidgetTree(QWidget* root);
    static QVector<MnemonicProblem> auditMenu(const QMenu* menu);
    static QVector<MnemonicProblem> auditMenus(const QMainWindow* window);

    static double contrastRatio(const QColor& foreground, const QColor& background);
    static bool meetsContrast(const QColor& foreground, const QColor& background, double requiredRatio);

    static void applyActionAccessibility(QToolButton* button, const QAction* action);
    static int minimumSpinBoxWidth(const QAbstractSpinBox* spinBox, const QString& sampleText);

    /// Registers the custom accessible interface used by PDFDrawWidget.
    /// Registration is process-wide and safe to call more than once.
    static void install();
};

} // namespace pdf

#endif // PDFACCESSIBILITY_H
