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

#ifndef PDFARTIFACTIDENTITY_H
#define PDFARTIFACTIDENTITY_H

#include "pdfglobal.h"

#include <QJsonObject>
#include <QString>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFArtifactIdentity
{
    QString sha256;
    qint64 size = -1;
    QString mediaType = QStringLiteral("application/pdf");
    QString logicalName;
    QString storageToken;

    bool isValid() const;
    QJsonObject toJson() const;
    static PDFArtifactIdentity fromJson(const QJsonObject& object);
};

LOOPLIBCORESHARED_EXPORT bool isPDFSha256(const QString& value);
LOOPLIBCORESHARED_EXPORT QString sanitizeArtifactLogicalName(const QString& value);
LOOPLIBCORESHARED_EXPORT QJsonValue canonicalizeJson(const QJsonValue& value);
LOOPLIBCORESHARED_EXPORT QByteArray canonicalJson(const QJsonValue& value);
LOOPLIBCORESHARED_EXPORT QJsonValue redactSensitiveJson(const QJsonValue& value);

} // namespace pdf

#endif // PDFARTIFACTIDENTITY_H
