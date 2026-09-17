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

#include "actionlistcatalog.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace pdfinteraction
{

ActionListCatalog::ActionListCatalog(QObject* parent) :
    QObject(parent)
{
}

QString ActionListCatalog::recipesDirectory() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("recipes"));
}

bool ActionListCatalog::ensureRecipesDirectory(QString* error) const
{
    const QString directory = recipesDirectory();
    if (QDir().mkpath(directory))
    {
        return true;
    }
    if (error)
    {
        *error = QStringLiteral("Unable to create Action List recipe directory '%1'.").arg(directory);
    }
    return false;
}

bool ActionListCatalog::loadRecipeFile(const QString& sourcePath, ActionListRecipeEntry* entry)
{
    if (!entry)
    {
        return false;
    }

    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly))
    {
        entry->id = sourcePath;
        entry->source = sourcePath;
        entry->name = QFileInfo(sourcePath).completeBaseName();
        entry->diagnostic = QStringLiteral("Unable to read Action List recipe.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
    {
        entry->id = sourcePath;
        entry->source = sourcePath;
        entry->name = QFileInfo(sourcePath).completeBaseName();
        entry->diagnostic = QStringLiteral("Action List recipe is not valid JSON: %1").arg(parseError.errorString());
        return false;
    }

    entry->id = sourcePath;
    entry->source = sourcePath;
    entry->name = QFileInfo(sourcePath).completeBaseName();
    if (const pdf::PDFOperationResult parseResult = pdf::PDFActionList::fromJson(parsed.object(), &entry->actionList); !parseResult)
    {
        entry->diagnostic = parseResult.getErrorMessage();
        return false;
    }

    entry->name = entry->actionList.name;
    pdf::PDFActionListExecutionOptions options;
    QStringList validationErrors;
    entry->valid = static_cast<bool>(pdf::PDFActionListExecutor().validate(entry->actionList, options, &validationErrors));
    entry->validationErrors = validationErrors;
    if (!entry->valid)
    {
        entry->diagnostic = validationErrors.join(QLatin1Char('\n'));
        return false;
    }

    pdf::PDFActionListExecutionResult planned;
    if (const pdf::PDFOperationResult planResult = pdf::PDFActionListExecutor().plan(entry->actionList, pdf::PDFDocument(), options, &planned); planResult)
    {
        entry->recipeHash = planned.recipeHash;
    }
    entry->diagnostic.clear();
    return true;
}

bool ActionListCatalog::reload()
{
    QList<ActionListRecipeEntry> recipes;
    const QString directory = recipesDirectory();
    const QDir local(directory);
    if (local.exists())
    {
        for (const QFileInfo& file : local.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name))
        {
            ActionListRecipeEntry entry;
            loadRecipeFile(file.absoluteFilePath(), &entry);
            recipes.append(std::move(entry));
        }
    }

    m_recipes = std::move(recipes);
    Q_EMIT recipesChanged();
    return true;
}

bool ActionListCatalog::importRecipe(const QString& sourcePath, QString* importedId, QString* error)
{
    const QFileInfo source(sourcePath);
    if (!source.exists() || !source.isFile())
    {
        if (error)
        {
            *error = QStringLiteral("Action List recipe '%1' does not exist.").arg(sourcePath);
        }
        return false;
    }

    QString directoryError;
    if (!ensureRecipesDirectory(&directoryError))
    {
        if (error)
        {
            *error = directoryError;
        }
        return false;
    }

    const QString destination = QDir(recipesDirectory()).filePath(source.fileName());
    if (QFileInfo(destination).absoluteFilePath() != source.absoluteFilePath())
    {
        if (QFile::exists(destination) && !QFile::remove(destination))
        {
            if (error)
            {
                *error = QStringLiteral("Unable to replace existing Action List recipe '%1'.").arg(destination);
            }
            return false;
        }
        if (!QFile::copy(source.absoluteFilePath(), destination))
        {
            if (error)
            {
                *error = QStringLiteral("Unable to import Action List recipe to '%1'.").arg(destination);
            }
            return false;
        }
    }

    reload();
    if (importedId)
    {
        *importedId = destination;
    }
    return true;
}

bool ActionListCatalog::exportRecipe(const QString& recipeId, const QString& destinationPath, QString* error)
{
    const ActionListRecipeEntry* entry = recipe(recipeId);
    if (!entry)
    {
        if (error)
        {
            *error = QStringLiteral("Action List recipe '%1' was not found.").arg(recipeId);
        }
        return false;
    }

    QFile output(destinationPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error)
        {
            *error = QStringLiteral("Unable to write Action List recipe to '%1'.").arg(destinationPath);
        }
        return false;
    }

    const QJsonDocument document(entry->actionList.toJson());
    if (output.write(document.toJson(QJsonDocument::Indented)) < 0)
    {
        if (error)
        {
            *error = QStringLiteral("Unable to serialize Action List recipe.");
        }
        return false;
    }
    return true;
}

bool ActionListCatalog::saveRecipe(const QString& recipeId, const pdf::PDFActionList& actionList, QString* error)
{
    const ActionListRecipeEntry* entry = recipe(recipeId);
    if (!entry)
    {
        if (error)
        {
            *error = QStringLiteral("Action List recipe '%1' was not found.").arg(recipeId);
        }
        return false;
    }

    QSaveFile output(entry->source);
    if (!output.open(QIODevice::WriteOnly))
    {
        if (error)
        {
            *error = QStringLiteral("Unable to write Action List recipe to '%1'.").arg(entry->source);
        }
        return false;
    }
    if (output.write(QJsonDocument(actionList.toJson()).toJson(QJsonDocument::Indented)) < 0 || !output.commit())
    {
        if (error)
        {
            *error = QStringLiteral("Unable to save Action List recipe '%1'.").arg(entry->source);
        }
        return false;
    }
    return true;
}

const ActionListRecipeEntry* ActionListCatalog::recipe(const QString& recipeId) const
{
    const auto it = std::find_if(m_recipes.cbegin(), m_recipes.cend(),
                                 [&recipeId](const ActionListRecipeEntry& entry)
                                 { return entry.id == recipeId; });
    return it == m_recipes.cend() ? nullptr : &(*it);
}

}   // namespace pdfinteraction
