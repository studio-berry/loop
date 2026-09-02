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

#include "documentcontextsource.h"

namespace pdfinteraction
{

PDFDocumentContextSource::PDFDocumentContextSource(pdf::PDFDocumentContext* context, QObject* parent) :
    QObject(parent),
    m_context(context)
{
    if (m_context)
    {
        connect(m_context, &pdf::PDFDocumentContext::revisionChanged,
                this, &PDFDocumentContextSource::revisionChanged);
    }
}

PDFDocumentContextSource::~PDFDocumentContextSource() = default;

pdf::PDFRevisionIdentity PDFDocumentContextSource::currentRevision() const
{
    if (!m_context)
    {
        return pdf::PDFRevisionIdentity();
    }

    return m_context->getRevision();
}

bool PDFDocumentContextSource::isCurrent(const pdf::PDFRevisionIdentity& revision) const
{
    if (!m_context)
    {
        // An unbound source has no current revision to match. Reporting "current"
        // here would admit a result whose document is already gone.
        return false;
    }

    return m_context->isCurrent(revision);
}

pdf::PDFDocumentContext* PDFDocumentContextSource::context() const
{
    return m_context.data();
}

bool PDFDocumentContextSource::isBound() const
{
    return !m_context.isNull();
}

QString PDFDocumentContextSource::documentKey() const
{
    if (!m_context)
    {
        return QString();
    }

    return m_context->getDocumentIdentity().documentId;
}

}   // namespace pdfinteraction
