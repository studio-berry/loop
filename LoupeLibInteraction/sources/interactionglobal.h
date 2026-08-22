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

#ifndef INTERACTIONGLOBAL_H
#define INTERACTIONGLOBAL_H

/// \namespace pdfinteraction
///
/// Host-neutral product interaction layer. It sits between LoupeLibCore, which
/// owns PDF truth, and whichever presentation host renders it.
///
/// The boundary is compile-time, not conventional. This target links neither
/// Qt6::Widgets nor Qt6::Qml/Quick, so Qt's per-module include paths are not on
/// the compiler's search path and a Widgets or QML include here fails to build.
/// scripts/verify-interaction-boundary.py additionally rejects a link edge or an
/// include that would reopen the seam, and architecture invariant I21 binds the
/// rule to UnitTestsInteractionBoundary.
///
/// This layer therefore must not own or expose QWidget, QDialog, QAction,
/// QQmlEngine, QQuickItem, or QSGNode. It must not introduce a second document
/// truth, scheduler, thread pool, revision identity, command registry, renderer,
/// or mutation route; pdf::PDFDocumentContext and pdf::PDFJobScheduler remain the
/// only ones. See docs/interaction-boundary-policy.json.

namespace pdfinteraction
{
}   // namespace pdfinteraction

#endif   // INTERACTIONGLOBAL_H
