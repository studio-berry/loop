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

struct LOOPLIBCORESHARED_EXPORT PDFOperationSavePolicy
{
    PDFSaveMode mode = PDFSaveMode::IncrementalAppend;
    bool invalidatesSignatures = false;
    bool reversibleInSession = true;
    QString rationale;

    static PDFOperationSavePolicy incrementalAppend(QString rationale = {});
    static PDFOperationSavePolicy fullRewrite(QString rationale = {});
    static PDFOperationSavePolicy saveAsNewArtifact(QString rationale = {});

    QJsonObject toJson() const;

    /// True when nobody declared this policy. The conservative default keeps
    /// an unclassified operation from being appended, but it is not a
    /// declaration: the registry-wide test rejects it, so a forgotten override
    /// is a visible defect instead of a silent fallback.
    bool isUndeclared() const;

    /// The conservative, explicitly-undeclared policy. Single source of the
    /// default; `PDFRepairOperation::savePolicy()` and `isUndeclared()` both
    /// resolve to it.
    static PDFOperationSavePolicy undeclared();
};

LOOPLIBCORESHARED_EXPORT const char* getPDFSaveModeName(PDFSaveMode mode);
LOOPLIBCORESHARED_EXPORT PDFOperationSavePolicy mergePDFSavePolicies(const PDFOperationSavePolicy& first,
                                                                     const PDFOperationSavePolicy& second);

/// True when \p candidate asks for less persistence safety than \p required:
/// a weaker mode, or - at the same mode - an unstated signature loss or a
/// claimed reversibility the operation does not have. Stricter policies are
/// allowed.
LOOPLIBCORESHARED_EXPORT bool savePolicyIsWeaker(const PDFOperationSavePolicy& candidate,
                                                 const PDFOperationSavePolicy& required);

/// The single refusal message for a weakened request, naming every weakening
/// reason the predicate finds. Empty when the request is not weaker, so
/// callers can use it as both the reason and the predicate.
LOOPLIBCORESHARED_EXPORT QString savePolicyWeakenedMessage(const PDFOperationSavePolicy& candidate,
                                                           const PDFOperationSavePolicy& required);

}   // namespace pdf

#endif   // PDFSAVEPOLICY_H
