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

#ifndef PDFINTERACTIONSTATE_P_H
#define PDFINTERACTIONSTATE_P_H

#include "pdfdocumentcontext.h"

#include <QtGlobal>

namespace pdf
{

/// Private, in-memory state for direct manipulation on the QWidget canvas.
///
/// The state owns no PDF data and is never serialized. A token is bound to the
/// complete document revision fence so deferred input work cannot act on a
/// document or cache generation that has since been superseded.
class PDFInteractionState final
{
public:
    enum class Kind
    {
        None,
        Hover,
        Drag,
        Marquee,
        ZoomPan,
        ToolGesture
    };

    enum class CancelReason
    {
        None,
        Explicit,
        Escape,
        FocusLost,
        RevisionChanged,
        DocumentReplaced,
        InvalidDrop,
        Destroyed
    };

    struct Token
    {
        quint64 generation = 0;
        PDFRevisionIdentity revision;

        bool isValid() const { return generation != 0; }
    };

    struct Snapshot
    {
        Kind kind = Kind::None;
        CancelReason lastCancelReason = CancelReason::None;
        Token token;

        bool active() const { return kind != Kind::None && token.isValid(); }
    };

    PDFInteractionState() = default;
    ~PDFInteractionState() = default;

    PDFInteractionState(const PDFInteractionState&) = delete;
    PDFInteractionState& operator=(const PDFInteractionState&) = delete;

    /// Starts a transient interaction, reusing the token when the kind and
    /// complete revision fence are unchanged.
    Token begin(Kind kind, const PDFRevisionIdentity& revision);

    /// Returns the token for the current active interaction, if any.
    Token currentToken() const { return m_token; }

    /// Returns whether the requested transient kind is currently active.
    bool isActive(Kind kind) const { return m_kind == kind && m_token.isValid(); }

    /// Returns a compact in-memory snapshot for tests and diagnostics.
    Snapshot snapshot() const;

    /// Returns whether a continuation belongs to the active interaction and
    /// the supplied revision is still current.
    bool isCurrent(const Token& token, const PDFRevisionIdentity& revision) const;

    /// Updates an active interaction without changing its token.
    bool update(const Token& token, const PDFRevisionIdentity& revision);

    /// Completes an active interaction if its token and revision still match.
    bool complete(const Token& token, const PDFRevisionIdentity& revision);

    /// Cancels the active interaction and invalidates its token.
    void cancel(CancelReason reason);

    /// Clears hover state without cancelling a drag or tool gesture.
    void clearHover();

private:
    Kind m_kind = Kind::None;
    CancelReason m_lastCancelReason = CancelReason::None;
    Token m_token;
    quint64 m_nextGeneration = 0;
};

}   // namespace pdf

#endif   // PDFINTERACTIONSTATE_P_H
