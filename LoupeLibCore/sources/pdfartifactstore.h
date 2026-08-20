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

#ifndef PDFARTIFACTSTORE_H
#define PDFARTIFACTSTORE_H

#include "pdfartifactidentity.h"

#include <QByteArray>
#include <QString>

class QIODevice;

namespace pdf
{

struct LOUPELIBCORESHARED_EXPORT PDFArtifactImportOptions
{
    QString mediaType = QStringLiteral("application/pdf");
    QString logicalName;
};

struct LOUPELIBCORESHARED_EXPORT PDFArtifactStoreResult
{
    bool success = false;
    bool reused = false;
    PDFArtifactIdentity artifact;
    QString errorMessage;
};

struct LOUPELIBCORESHARED_EXPORT PDFArtifactRestoreResult
{
    bool success = false;
    QString errorMessage;
};

class LOUPELIBCORESHARED_EXPORT PDFArtifactStore
{
public:
    explicit PDFArtifactStore(QString rootDirectory);

    const QString& rootDirectory() const { return m_rootDirectory; }

    PDFArtifactStoreResult importFile(const QString& sourcePath,
                                      PDFArtifactImportOptions options = {}) const;
    PDFArtifactStoreResult importBytes(const QByteArray& bytes,
                                       PDFArtifactImportOptions options = {}) const;

    QString pathFor(const PDFArtifactIdentity& artifact) const;
    bool contains(const PDFArtifactIdentity& artifact) const;
    bool verify(const PDFArtifactIdentity& artifact) const;
    /// Removes exactly one digest-addressed artifact. Callers must first decide
    /// retention eligibility in the operation-history ledger.
    bool remove(const PDFArtifactIdentity& artifact) const;
    PDFArtifactRestoreResult restoreToFile(const PDFArtifactIdentity& artifact,
                                           const QString& destinationPath) const;

private:
    PDFArtifactStoreResult importDevice(QIODevice* source,
                                        const PDFArtifactImportOptions& options) const;
    bool publishReadOnly(const QString& path) const;
    QString artifactToken(const QString& sha256) const;

    QString m_rootDirectory;
};

} // namespace pdf

#endif // PDFARTIFACTSTORE_H
