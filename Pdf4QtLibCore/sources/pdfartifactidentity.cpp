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

#include "pdfartifactidentity.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

namespace pdf
{

namespace
{

bool isSensitiveKey(const QString& key)
{
    const QString normalized = key.toLower();
    for (const QString& token : { QStringLiteral("password"), QStringLiteral("passwd"), QStringLiteral("pswd"),
                                  QStringLiteral("secret"), QStringLiteral("token"), QStringLiteral("api_key"),
                                  QStringLiteral("apikey"), QStringLiteral("dsn"), QStringLiteral("license"),
                                  QStringLiteral("filepath"), QStringLiteral("sourcepath"), QStringLiteral("outputpath"),
                                  QStringLiteral("directory"), QStringLiteral("path") })
    {
        if (normalized.contains(token))
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool isPDFSha256(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return expression.match(value).hasMatch();
}

QString sanitizeArtifactLogicalName(const QString& value)
{
    QString name = QFileInfo(value).fileName().trimmed();
    name.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1F\\x7F]")), QStringLiteral("_"));
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("_"));
    name = name.left(255).trimmed();
    return name.isEmpty() ? QStringLiteral("artifact") : name;
}

bool PDFArtifactIdentity::isValid() const
{
    return isPDFSha256(sha256) && size >= 0 && !mediaType.trimmed().isEmpty() &&
           !logicalName.contains(QChar('\0')) && !storageToken.contains(QStringLiteral(".."));
}

QJsonObject PDFArtifactIdentity::toJson() const
{
    return QJsonObject{
        { QStringLiteral("sha256"), sha256 },
        { QStringLiteral("size"), size },
        { QStringLiteral("mediaType"), mediaType },
        { QStringLiteral("logicalName"), sanitizeArtifactLogicalName(logicalName) },
        { QStringLiteral("storageToken"), storageToken }
    };
}

PDFArtifactIdentity PDFArtifactIdentity::fromJson(const QJsonObject& object)
{
    PDFArtifactIdentity identity;
    identity.sha256 = object.value(QStringLiteral("sha256")).toString().toLower();
    identity.size = object.value(QStringLiteral("size")).toInteger(-1);
    identity.mediaType = object.value(QStringLiteral("mediaType")).toString(QStringLiteral("application/pdf"));
    identity.logicalName = sanitizeArtifactLogicalName(object.value(QStringLiteral("logicalName")).toString());
    identity.storageToken = object.value(QStringLiteral("storageToken")).toString();
    return identity;
}

QJsonValue canonicalizeJson(const QJsonValue& value)
{
    if (value.isObject())
    {
        QJsonObject result;
        QStringList keys = value.toObject().keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& key : keys)
        {
            result.insert(key, canonicalizeJson(value.toObject().value(key)));
        }
        return result;
    }
    if (value.isArray())
    {
        QJsonArray result;
        for (const QJsonValue& item : value.toArray())
        {
            result.append(canonicalizeJson(item));
        }
        return result;
    }
    return value;
}

QByteArray canonicalJson(const QJsonValue& value)
{
    const QJsonValue canonical = canonicalizeJson(value);
    if (canonical.isObject())
    {
        return QJsonDocument(canonical.toObject()).toJson(QJsonDocument::Compact);
    }
    if (canonical.isArray())
    {
        return QJsonDocument(canonical.toArray()).toJson(QJsonDocument::Compact);
    }
    return QJsonDocument(QJsonArray{ canonical }).toJson(QJsonDocument::Compact);
}

QJsonValue redactSensitiveJson(const QJsonValue& value)
{
    if (value.isObject())
    {
        QJsonObject result;
        const QJsonObject object = value.toObject();
        for (auto it = object.cbegin(); it != object.cend(); ++it)
        {
            result.insert(it.key(), isSensitiveKey(it.key())
                          ? QJsonValue(QStringLiteral("[REDACTED]"))
                          : redactSensitiveJson(it.value()));
        }
        return result;
    }
    if (value.isArray())
    {
        QJsonArray result;
        for (const QJsonValue& item : value.toArray())
        {
            result.append(redactSensitiveJson(item));
        }
        return result;
    }
    return value;
}

} // namespace pdf
