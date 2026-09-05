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

#include "pdfdocumentcontext.h"

#include "pdfdocumentsession.h"

#include <tuple>
#include <utility>

namespace pdf
{

PDFDocumentIdentity PDFDocumentIdentity::fromDocument(const PDFDocument* document)
{
    if (!document)
    {
        return PDFDocumentIdentity();
    }

    PDFDocumentIdentity identity;
    identity.sourceDataHash = document->getSourceDataHash();
    identity.documentId = QString::number(reinterpret_cast<quintptr>(document), 16);
    return identity;
}

bool PDFRevisionIdentity::operator<(const PDFRevisionIdentity& other) const
{
    return std::tie(document.sourceDataHash, document.documentId, documentRevision, cacheGeneration, effectiveProfileIdentity) <
           std::tie(other.document.sourceDataHash, other.document.documentId, other.documentRevision, other.cacheGeneration, other.effectiveProfileIdentity);
}

QString PDFRevisionIdentity::toString() const
{
    return QStringLiteral("%1:%2:%3:%4:%5")
        .arg(QString::fromLatin1(document.sourceDataHash.toHex()))
        .arg(document.documentId)
        .arg(documentRevision)
        .arg(cacheGeneration)
        .arg(effectiveProfileIdentity);
}

PDFDocumentContext::PDFDocumentContext(PDFDocument* document, QObject* parent) :
    QObject(parent),
    m_document(document),
    m_documentIdentity(PDFDocumentIdentity::fromDocument(document)),
    m_pageCacheBudget(std::make_shared<PDFPageCacheBudget>())
{
    if (document != nullptr)
    {
        m_session = std::make_unique<PDFDocumentSession>(document, this, m_pageCacheBudget);
    }
}

PDFDocumentContext::PDFDocumentContext(PDFDocumentPointer document, QObject* parent) :
    QObject(parent),
    m_documentPointer(std::move(document)),
    m_document(m_documentPointer.data()),
    m_documentIdentity(PDFDocumentIdentity::fromDocument(m_document)),
    m_pageCacheBudget(std::make_shared<PDFPageCacheBudget>())
{
    if (m_document != nullptr)
    {
        m_session = std::make_unique<PDFDocumentSession>(m_document, this, m_pageCacheBudget);
    }
}

PDFDocumentContext::~PDFDocumentContext() = default;

void PDFDocumentContext::ensureSession()
{
    if (!m_session)
    {
        m_session = std::make_unique<PDFDocumentSession>(m_document, this, m_pageCacheBudget);
    }
}

PDFDocumentSession* PDFDocumentContext::getSession()
{
    ensureSession();
    return m_session.get();
}

const PDFDocumentSession* PDFDocumentContext::getSession() const
{
    const_cast<PDFDocumentContext*>(this)->ensureSession();
    return m_session.get();
}

PDFRevisionIdentity PDFDocumentContext::getRevision() const
{
    PDFRevisionIdentity revision;
    revision.document = m_documentIdentity;
    revision.documentRevision = m_documentRevision;
    revision.cacheGeneration = m_cacheGeneration;
    revision.effectiveProfileIdentity = m_effectiveProfileIdentity;
    return revision;
}

void PDFDocumentContext::setDocument(PDFDocument* document, PDFModifiedDocument::ModificationFlags flags)
{
    if (document != m_document)
    {
        replaceDocument(document, PDFDocumentPointer());
    }
    else if (flags != PDFModifiedDocument::None)
    {
        markModified(flags);
    }
}

void PDFDocumentContext::setDocument(PDFDocumentPointer document, PDFModifiedDocument::ModificationFlags flags)
{
    // The raw pointer is read before the move, not inside the same call. Argument
    // evaluation order is unspecified, so replaceDocument(document.data(),
    // std::move(document)) may move the owner out first and then read data() from
    // an already-null pointer, leaving the context owning a document it reports as
    // absent. MSVC evaluates right to left and does exactly that.
    PDFDocument* rawDocument = document.data();

    if (rawDocument != m_document)
    {
        replaceDocument(rawDocument, std::move(document));
    }
    else if (flags != PDFModifiedDocument::None)
    {
        markModified(flags);
    }
}

void PDFDocumentContext::markModified(PDFModifiedDocument::ModificationFlags flags)
{
    if (flags == PDFModifiedDocument::None)
    {
        return;
    }

    const PDFRevisionIdentity previous = getRevision();
    ++m_documentRevision;
    ++m_cacheGeneration;
    if (m_session)
    {
        m_session->invalidate();
    }
    emitRevisionChanged(previous);
}

void PDFDocumentContext::setEffectiveProfileIdentity(QString identity)
{
    if (m_effectiveProfileIdentity == identity)
    {
        return;
    }

    const PDFRevisionIdentity previous = getRevision();
    m_effectiveProfileIdentity = std::move(identity);
    ++m_cacheGeneration;
    if (m_session)
    {
        m_session->invalidate();
    }
    emitRevisionChanged(previous);
}

void PDFDocumentContext::invalidateCaches()
{
    const PDFRevisionIdentity previous = getRevision();
    ++m_cacheGeneration;
    if (m_session)
    {
        m_session->invalidate();
    }
    emitRevisionChanged(previous);
}

void PDFDocumentContext::replaceDocument(PDFDocument* document, PDFDocumentPointer owner)
{
    const PDFRevisionIdentity previous = getRevision();

    std::unique_ptr<PDFDocumentSession> nextSession;
    try
    {
        nextSession = std::make_unique<PDFDocumentSession>(document, this, m_pageCacheBudget);
    }
    catch (const PDFResourceBudgetExceededException&)
    {
        // The new document's model does not fit in the resource budget; leave
        // the existing session and document intact so the caller can report the
        // failure without crashing the application.
        throw;
    }

    m_documentPointer = std::move(owner);
    m_document = document;
    m_documentIdentity = PDFDocumentIdentity::fromDocument(document);
    ++m_documentRevision;
    ++m_cacheGeneration;
    m_session = std::move(nextSession);
    emitRevisionChanged(previous);
}

void PDFDocumentContext::emitRevisionChanged(const PDFRevisionIdentity& previous)
{
    Q_EMIT revisionChanged(previous, getRevision());
}

}   // namespace pdf
