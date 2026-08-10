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

#include "pdfsavepolicy.h"

#include <utility>

namespace pdf
{

const char* getPDFSaveModeName(PDFSaveMode mode)
{
    switch (mode)
    {
        case PDFSaveMode::IncrementalAppend: return "incremental-append";
        case PDFSaveMode::FullRewrite: return "full-rewrite";
        case PDFSaveMode::SaveAsNewArtifact: return "save-as-new-artifact";
    }
    return "unknown";
}

PDFOperationSavePolicy PDFOperationSavePolicy::incrementalAppend(QString rationale)
{
    PDFOperationSavePolicy policy;
    policy.mode = PDFSaveMode::IncrementalAppend;
    policy.rationale = std::move(rationale);
    return policy;
}

PDFOperationSavePolicy PDFOperationSavePolicy::fullRewrite(QString rationale)
{
    PDFOperationSavePolicy policy;
    policy.mode = PDFSaveMode::FullRewrite;
    policy.invalidatesSignatures = true;
    policy.reversibleInSession = false;
    policy.rationale = std::move(rationale);
    return policy;
}

PDFOperationSavePolicy PDFOperationSavePolicy::saveAsNewArtifact(QString rationale)
{
    PDFOperationSavePolicy policy;
    policy.mode = PDFSaveMode::SaveAsNewArtifact;
    policy.invalidatesSignatures = true;
    policy.reversibleInSession = true;
    policy.rationale = std::move(rationale);
    return policy;
}

QJsonObject PDFOperationSavePolicy::toJson() const
{
    return QJsonObject{
        { QStringLiteral("mode"), QString::fromLatin1(getPDFSaveModeName(mode)) },
        { QStringLiteral("invalidates_signatures"), invalidatesSignatures },
        { QStringLiteral("reversible_in_session"), reversibleInSession },
        { QStringLiteral("rationale"), rationale }
    };
}

PDFOperationSavePolicy mergePDFSavePolicies(const PDFOperationSavePolicy& first,
                                            const PDFOperationSavePolicy& second)
{
    PDFOperationSavePolicy result = first;
    if (static_cast<int>(second.mode) > static_cast<int>(result.mode))
    {
        result.mode = second.mode;
    }
    result.invalidatesSignatures = result.invalidatesSignatures || second.invalidatesSignatures;
    result.reversibleInSession = result.reversibleInSession && second.reversibleInSession;
    if (!second.rationale.isEmpty())
    {
        if (!result.rationale.isEmpty())
        {
            result.rationale += QStringLiteral("; ");
        }
        result.rationale += second.rationale;
    }
    return result;
}

} // namespace pdf
