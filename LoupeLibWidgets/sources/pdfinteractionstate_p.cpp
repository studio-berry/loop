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

#include "pdfinteractionstate_p.h"

namespace pdf
{

PDFInteractionState::Token PDFInteractionState::begin(Kind kind, const PDFRevisionIdentity& revision)
{
    if (kind == Kind::None)
    {
        cancel(CancelReason::Explicit);
        return {};
    }

    if (m_kind == kind && m_token.revision == revision && m_token.isValid())
    {
        return m_token;
    }

    if (m_kind != Kind::None)
    {
        cancel(CancelReason::Explicit);
    }

    m_kind = kind;
    m_lastCancelReason = CancelReason::None;
    m_token = Token{ ++m_nextGeneration, revision };
    return m_token;
}

PDFInteractionState::Snapshot PDFInteractionState::snapshot() const
{
    return Snapshot{ m_kind, m_lastCancelReason, m_token };
}

bool PDFInteractionState::isCurrent(const Token& token, const PDFRevisionIdentity& revision) const
{
    return m_kind != Kind::None && token.isValid() && token.generation == m_token.generation && token.revision == m_token.revision && revision == m_token.revision;
}

bool PDFInteractionState::update(const Token& token, const PDFRevisionIdentity& revision)
{
    return isCurrent(token, revision);
}

bool PDFInteractionState::complete(const Token& token, const PDFRevisionIdentity& revision)
{
    if (!isCurrent(token, revision))
    {
        return false;
    }

    m_kind = Kind::None;
    m_token = Token{};
    m_lastCancelReason = CancelReason::None;
    return true;
}

void PDFInteractionState::cancel(CancelReason reason)
{
    if (m_kind == Kind::None && !m_token.isValid())
    {
        m_lastCancelReason = reason;
        return;
    }

    m_kind = Kind::None;
    m_token = Token{};
    m_lastCancelReason = reason;
    ++m_nextGeneration;
}

void PDFInteractionState::clearHover()
{
    if (m_kind == Kind::Hover)
    {
        m_kind = Kind::None;
        m_token = Token{};
    }
}

}   // namespace pdf
