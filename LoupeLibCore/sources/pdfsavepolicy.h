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

#ifndef PDFSAVEPOLICY_H
#define PDFSAVEPOLICY_H

#include "pdfglobal.h"

#include <QJsonObject>
#include <QString>

namespace pdf
{

enum class PDFSaveMode
{
    IncrementalAppend,
    FullRewrite,
    SaveAsNewArtifact
};

struct LOUPELIBCORESHARED_EXPORT PDFOperationSavePolicy
{
    PDFSaveMode mode = PDFSaveMode::IncrementalAppend;
    bool invalidatesSignatures = false;
    bool reversibleInSession = true;
    QString rationale;

    static PDFOperationSavePolicy incrementalAppend(QString rationale = {});
    static PDFOperationSavePolicy fullRewrite(QString rationale = {});
    static PDFOperationSavePolicy saveAsNewArtifact(QString rationale = {});

    QJsonObject toJson() const;
};

LOUPELIBCORESHARED_EXPORT const char* getPDFSaveModeName(PDFSaveMode mode);
LOUPELIBCORESHARED_EXPORT PDFOperationSavePolicy mergePDFSavePolicies(const PDFOperationSavePolicy& first,
                                                                         const PDFOperationSavePolicy& second);

} // namespace pdf

#endif // PDFSAVEPOLICY_H
