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

#ifndef PDFDOCUMENTCONTEXT_H
#define PDFDOCUMENTCONTEXT_H

#include "pdfdocument.h"
#include "pdfglobal.h"
#include "pdfpagecachebudget.h"

#include <QObject>

#include <memory>

namespace pdf
{

class PDFDocumentSession;

using DocumentRevision = quint64;

/// Stable identity for the in-session document currently held by a context.
/// Distinct from pdf::PDFArtifactIdentity in pdfartifactidentity.h, which
/// identifies a *persisted* artifact by content hash and storage token. Both
/// live in namespace pdf and in the LoopLibCore target, so they must not
/// share a name — see UnitTests/tst_identityseparationtest.cpp.
struct LOOPLIBCORESHARED_EXPORT PDFDocumentIdentity
{
    QByteArray sourceDataHash;
    QString documentId;

    bool isValid() const { return !documentId.isEmpty() || !sourceDataHash.isEmpty(); }
    bool operator==(const PDFDocumentIdentity&) const = default;

    static PDFDocumentIdentity fromDocument(const PDFDocument* document);
};

/// Revision fence shared by document caches, findings, and asynchronous jobs.
/// A result is usable only when this complete value still equals the context's
/// current value. The cache generation changes for renderer/profile changes;
/// documentRevision changes for document mutations or replacement.
struct LOOPLIBCORESHARED_EXPORT PDFRevisionIdentity
{
    PDFDocumentIdentity document;
    DocumentRevision documentRevision = 0;
    quint64 cacheGeneration = 0;
    QString effectiveProfileIdentity;

    bool isValid() const { return document.isValid(); }
    bool operator==(const PDFRevisionIdentity&) const = default;
    bool operator<(const PDFRevisionIdentity& other) const;
    QString toString() const;
};

using PDFRevisionToken = PDFRevisionIdentity;

/// Owns the active document identity and the one revision fence used by
/// document-bound caches and asynchronous work.
class LOOPLIBCORESHARED_EXPORT PDFDocumentContext : public QObject
{
    Q_OBJECT

public:
    explicit PDFDocumentContext(PDFDocument* document, QObject* parent = nullptr);
    explicit PDFDocumentContext(PDFDocumentPointer document, QObject* parent = nullptr);
    ~PDFDocumentContext() override;

    PDFDocumentContext(const PDFDocumentContext&) = delete;
    PDFDocumentContext& operator=(const PDFDocumentContext&) = delete;

    PDFDocument* getDocument() const { return m_document; }
    PDFDocumentPointer getDocumentPointer() const { return m_documentPointer; }
    PDFDocumentIdentity getDocumentIdentity() const { return m_documentIdentity; }
    PDFRevisionIdentity getRevision() const;
    PDFDocumentSession* getSession() const { return m_session.get(); }
    PDFPageCacheBudget* getPageCacheBudget() const { return m_pageCacheBudget.get(); }
    std::shared_ptr<PDFPageCacheBudget> getSharedPageCacheBudget() const { return m_pageCacheBudget; }

    /// Returns whether a cache or job result belongs to the active revision.
    bool isCurrent(const PDFRevisionIdentity& revision) const { return revision == getRevision(); }

    /// Replaces the active document, or marks the current document modified when
    /// the pointer is unchanged. None is a no-op; all other flags advance the
    /// revision and invalidate the session-owned caches.
    void setDocument(PDFDocument* document, PDFModifiedDocument::ModificationFlags flags = PDFModifiedDocument::Reset);
    void setDocument(PDFDocumentPointer document, PDFModifiedDocument::ModificationFlags flags = PDFModifiedDocument::Reset);
    void markModified(PDFModifiedDocument::ModificationFlags flags = PDFModifiedDocument::Reset);

    /// Changes the effective profile/policy identity without pretending that the
    /// PDF bytes changed. This still fences all profile-dependent cache entries.
    void setEffectiveProfileIdentity(QString identity);

    /// Advances only the cache generation for renderer and processing settings.
    void invalidateCaches();

signals:
    void revisionChanged(const pdf::PDFRevisionIdentity& previous, const pdf::PDFRevisionIdentity& current);

private:
    void replaceDocument(PDFDocument* document, PDFDocumentPointer owner);
    void emitRevisionChanged(const PDFRevisionIdentity& previous);

    PDFDocumentPointer m_documentPointer;
    PDFDocument* m_document = nullptr;
    PDFDocumentIdentity m_documentIdentity;
    DocumentRevision m_documentRevision = 0;
    quint64 m_cacheGeneration = 0;
    QString m_effectiveProfileIdentity;
    std::shared_ptr<PDFPageCacheBudget> m_pageCacheBudget;
    std::unique_ptr<PDFDocumentSession> m_session;
};

}   // namespace pdf

Q_DECLARE_METATYPE(pdf::PDFRevisionIdentity)

#endif   // PDFDOCUMENTCONTEXT_H
