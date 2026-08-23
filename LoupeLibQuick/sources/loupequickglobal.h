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


#ifndef LOUPEQUICKGLOBAL_H
#define LOUPEQUICKGLOBAL_H

#include "loupelibquick_export.h"

/// \namespace pdfquick
///
/// The Qt Quick presentation host for the canvas (ADR-010, Phase 4 P4-S5).
///
/// This layer is the mirror image of pdfinteraction. That layer may not name a
/// QQuickItem or a QSGNode; this one exists to own them. Everything here is
/// presentation: scene-graph nodes, visual treatment for the neutral overlay
/// semantics, and the translation of Qt Quick's event objects into the
/// PointerIntent, WheelIntent and KeyIntent values InteractionController takes.
///
/// What this layer must never acquire, per ADR-010 rule 5:
///
///   - a second document truth. It holds no pdf::PDFDocument and no
///     pdf::PDFDocumentSession, and it never mutates one. Mutation stays behind
///     P4-S2's CommandCatalog.
///   - a second revision fence. It reads RevisionFencedToken and refuses what no
///     longer matches; it never invents a generation.
///   - a QML-visible document, session, scheduler, or pixel buffer.
///     SurfaceBuffer crosses C++ ownership boundaries only -- never a QML
///     property, JS value, context property, or URL. The QML-visible surface of
///     LoupeCanvasItem is presentation state and nothing else.
///
/// ADR-009 as amended admits the direct QQuickItem and prohibits
/// QQuickPaintedItem, QQuickWidget and WindowContainer as shipped product
/// architecture. LoupeCanvasItem is a QQuickItem that builds scene-graph nodes
/// in updatePaintNode; there is no paint bridge and no QPainter in the page
/// pixel or overlay path.

namespace pdfquick
{
}   // namespace pdfquick

#endif   // LOUPEQUICKGLOBAL_H
