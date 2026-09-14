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

#ifndef ACTIONLISTCATALOG_H
#define ACTIONLISTCATALOG_H

#include "interactionglobal.h"

#include "pdfactionlist.h"

#include <QList>
#include <QObject>
#include <QStringList>

namespace pdfinteraction
{

struct ActionListRecipeEntry
{
    QString id;
    QString name;
    QString source;
    QString diagnostic;
    QString recipeHash;
    bool valid = false;
    pdf::PDFActionList actionList;
    QStringList validationErrors;
};

class ActionListCatalog final : public QObject
{
    Q_OBJECT

public:
    explicit ActionListCatalog(QObject* parent = nullptr);

    const QList<ActionListRecipeEntry>& recipes() const noexcept { return m_recipes; }
    QString recipesDirectory() const;

    bool reload();
    bool importRecipe(const QString& sourcePath, QString* importedId = nullptr, QString* error = nullptr);
    bool exportRecipe(const QString& recipeId, const QString& destinationPath, QString* error = nullptr);
    const ActionListRecipeEntry* recipe(const QString& recipeId) const;

signals:
    void recipesChanged();

private:
    bool ensureRecipesDirectory(QString* error = nullptr) const;
    bool loadRecipeFile(const QString& sourcePath, ActionListRecipeEntry* entry);

    QList<ActionListRecipeEntry> m_recipes;
};

}   // namespace pdfinteraction

#endif   // ACTIONLISTCATALOG_H
