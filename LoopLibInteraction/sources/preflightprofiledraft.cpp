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

#include "preflightprofiledraft.h"

namespace pdfinteraction
{

namespace
{

bool isEditableScalarField(const QString& field)
{
    return field == QStringLiteral("severity") || field == QStringLiteral("enabled") || field == QStringLiteral("min_dpi") ||
           field == QStringLiteral("amount_pt") || field == QStringLiteral("tolerance_pt") || field == QStringLiteral("target_dpi") ||
           field == QStringLiteral("probe_dpi") || field == QStringLiteral("rich_black_k_percent") || field == QStringLiteral("required");
}

QVariant jsonValueToVariant(const QJsonValue& value)
{
    if (value.isBool())
    {
        return value.toBool();
    }
    if (value.isDouble())
    {
        return value.toDouble();
    }
    return value.toString();
}

QJsonValue variantToJsonValue(const QVariant& value, const QString& field)
{
    if (field == QStringLiteral("enabled") || field == QStringLiteral("required"))
    {
        return value.toBool();
    }
    if (field == QStringLiteral("severity"))
    {
        return value.toString();
    }
    if (value.typeId() == QMetaType::QString)
    {
        const QString text = value.toString();
        bool ok = false;
        const double number = text.toDouble(&ok);
        if (ok)
        {
            return number;
        }
        return text;
    }
    if (value.canConvert<double>())
    {
        return value.toDouble();
    }
    return QJsonValue::fromVariant(value);
}

QString bumpPatchVersion(const QString& version)
{
    const QStringList parts = version.split(QLatin1Char('.'));
    if (parts.size() != 3)
    {
        return QStringLiteral("1.0.1");
    }
    bool ok = false;
    int patch = parts.at(2).toInt(&ok);
    if (!ok)
    {
        return QStringLiteral("1.0.1");
    }
    return QStringLiteral("%1.%2.%3").arg(parts.at(0), parts.at(1), QString::number(patch + 1));
}

}   // namespace

bool PreflightProfileDraft::load(const QJsonObject& parentProfile, const pdf::PreflightProfileIdentity& parentIdentity)
{
    clear();
    if (parentProfile.isEmpty())
    {
        return false;
    }
    m_parentProfile = parentProfile;
    m_parentIdentity = parentIdentity;
    m_draftProfile = parentProfile;
    m_draftProfile.remove(QStringLiteral("digest"));
    m_active = true;
    m_dirty = false;
    return true;
}

void PreflightProfileDraft::clear()
{
    m_active = false;
    m_dirty = false;
    m_parentProfile = QJsonObject();
    m_draftProfile = QJsonObject();
    m_parentIdentity = pdf::PreflightProfileIdentity();
}

QString PreflightProfileDraft::suggestedNextVersion() const
{
    return bumpPatchVersion(m_parentIdentity.version.isEmpty() ? QStringLiteral("1.0.0") : m_parentIdentity.version);
}

QVariantList PreflightProfileDraft::editableChecks() const
{
    QVariantList checks;
    if (!m_active)
    {
        return checks;
    }

    const QJsonArray checkArray = m_draftProfile.value(QStringLiteral("checks")).toArray();
    for (const QJsonValueConstRef item : checkArray)
    {
        const QJsonObject check = item.toObject();
        const QString checkId = check.value(QStringLiteral("id")).toString();
        if (checkId.isEmpty())
        {
            continue;
        }

        QVariantMap row;
        row.insert(QStringLiteral("id"), checkId);
        row.insert(QStringLiteral("severity"), check.value(QStringLiteral("severity")).toString(QStringLiteral("warning")));
        row.insert(QStringLiteral("enabled"), check.contains(QStringLiteral("enabled")) ? check.value(QStringLiteral("enabled")).toBool(true) : true);

        QVariantList fields;
        for (auto it = check.constBegin(); it != check.constEnd(); ++it)
        {
            if (!isEditableScalarField(it.key()))
            {
                continue;
            }
            if (it.key() == QStringLiteral("severity") || it.key() == QStringLiteral("enabled"))
            {
                continue;
            }
            QVariantMap field;
            field.insert(QStringLiteral("name"), it.key());
            field.insert(QStringLiteral("value"), jsonValueToVariant(it.value()));
            fields.append(field);
        }
        row.insert(QStringLiteral("fields"), fields);
        checks.append(row);
    }
    return checks;
}

bool PreflightProfileDraft::setCheckField(const QString& checkId, const QString& field, const QVariant& value)
{
    if (!m_active || checkId.isEmpty() || field.isEmpty())
    {
        return false;
    }

    QJsonArray checks = m_draftProfile.value(QStringLiteral("checks")).toArray();
    for (int i = 0; i < checks.size(); ++i)
    {
        QJsonObject check = checks.at(i).toObject();
        if (check.value(QStringLiteral("id")).toString() != checkId)
        {
            continue;
        }
        if (!isEditableScalarField(field) && field != QStringLiteral("id"))
        {
            return false;
        }
        check.insert(field, variantToJsonValue(value, field));
        checks.replace(i, check);
        m_draftProfile.insert(QStringLiteral("checks"), checks);
        m_dirty = true;
        return true;
    }
    return false;
}

QJsonObject PreflightProfileDraft::commit(const QString& newVersion) const
{
    if (!m_active || newVersion.isEmpty())
    {
        return QJsonObject();
    }
    return pdf::commitPreflightProfileEdit(m_parentProfile, m_draftProfile, newVersion);
}

}   // namespace pdfinteraction
