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

#ifndef DOCUMENTLOADER_H
#define DOCUMENTLOADER_H

#include "interactionglobal.h"

#include "pdfdocument.h"
#include "pdfjobscheduler.h"
#include "pdfprocessingbudget.h"

#include <QString>

#include <functional>

namespace pdfinteraction
{

/// Where a document comes from.
///
/// `path` is the canonical file path and stays in C++. `displayLabel()` — the
/// file name alone — is the only part a presentation host may show, so a Quick
/// shell never receives a filesystem path it could hand back to a file service.
/// See docs/QUICK_SHELL_THREAT_MODEL.md.
struct DocumentSource
{
    QString path;

    bool isValid() const { return !path.isEmpty(); }
    QString displayLabel() const;
    bool operator==(const DocumentSource&) const = default;
};

enum class DocumentLoadOutcome
{
    Loaded,
    Failed,
    Cancelled
};

struct DocumentLoadResult
{
    DocumentLoadOutcome outcome = DocumentLoadOutcome::Failed;
    pdf::PDFDocumentPointer document;

    /// Policy-safe error code. Never a path, a password, or document content.
    QString typedError;

    /// The document parsed, but the reader could not honour everything it found.
    bool incomplete = false;

    /// The document requires something this build does not support.
    bool unsupported = false;
};

/// Reads a document. The seam exists so lifecycle tests can drive success,
/// failure, and cancellation without a file on disk, and so password prompting
/// stays a callback rather than a dialog the neutral layer would have to own.
class IDocumentLoader
{
public:
    virtual ~IDocumentLoader() = default;

    /// Called on a worker thread owned by pdf::PDFJobScheduler. Implementations
    /// must honour context.isCancellationRequested() so that closing a document
    /// with a read in flight terminates promptly.
    virtual DocumentLoadResult load(const DocumentSource& source, pdf::PDFJobContext& context) = 0;
};

enum class DocumentWriteOutcome
{
    Written,
    Failed,
    Cancelled
};

struct DocumentWriteResult
{
    DocumentWriteOutcome outcome = DocumentWriteOutcome::Failed;
    QString typedError;
};

/// Writes a document. Same rationale as IDocumentLoader.
class IDocumentWriter
{
public:
    virtual ~IDocumentWriter() = default;

    /// Called on a worker thread owned by pdf::PDFJobScheduler.
    virtual DocumentWriteResult write(const DocumentSource& target,
                                      const pdf::PDFDocument* document,
                                      pdf::PDFJobContext& context) = 0;
};

/// Adapts IDocumentLoader to pdf::PDFDocumentReader.
///
/// No second parser and no second processing-limit policy: the reader, its
/// permissive/authorization flags, and pdf::PDFProcessingLimits are Core's and
/// are passed through unchanged.
class PDFReaderDocumentLoader final : public IDocumentLoader
{
public:
    /// \param queryPassword Invoked on the worker thread when the document is
    ///        encrypted. A host that cannot prompt returns an empty string with
    ///        *ok set to false, which the reader reports as cancellation.
    explicit PDFReaderDocumentLoader(
        std::function<QString(bool*)> queryPassword = {},
        pdf::PDFProcessingLimits processingLimits = pdf::PDFProcessingLimits::conservativeDefaults());

    DocumentLoadResult load(const DocumentSource& source, pdf::PDFJobContext& context) override;

private:
    std::function<QString(bool*)> m_queryPassword;
    pdf::PDFProcessingLimits m_processingLimits;
};

/// Adapts IDocumentWriter to pdf::PDFDocumentWriter, always with the safe
/// temporary-file-and-rename path so a failed write cannot truncate the
/// operator's original file.
class PDFDocumentFileWriter final : public IDocumentWriter
{
public:
    DocumentWriteResult write(const DocumentSource& target,
                              const pdf::PDFDocument* document,
                              pdf::PDFJobContext& context) override;
};

}   // namespace pdfinteraction

#endif   // DOCUMENTLOADER_H
