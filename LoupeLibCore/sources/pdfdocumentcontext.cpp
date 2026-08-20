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
    m_session(std::make_unique<PDFDocumentSession>(document, this))
{
}

PDFDocumentContext::PDFDocumentContext(PDFDocumentPointer document, QObject* parent) :
    QObject(parent),
    m_documentPointer(std::move(document)),
    m_document(m_documentPointer.data()),
    m_documentIdentity(PDFDocumentIdentity::fromDocument(m_document)),
    m_session(std::make_unique<PDFDocumentSession>(m_document, this))
{
}

PDFDocumentContext::~PDFDocumentContext() = default;

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
    if (document.data() != m_document)
    {
        replaceDocument(document.data(), std::move(document));
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
    m_documentPointer = std::move(owner);
    m_document = document;
    m_documentIdentity = PDFDocumentIdentity::fromDocument(document);
    ++m_documentRevision;
    ++m_cacheGeneration;
    m_session = std::make_unique<PDFDocumentSession>(m_document, this);
    emitRevisionChanged(previous);
}

void PDFDocumentContext::emitRevisionChanged(const PDFRevisionIdentity& previous)
{
    Q_EMIT revisionChanged(previous, getRevision());
}

} // namespace pdf
