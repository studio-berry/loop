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

#ifndef PDFDRAWSPACELAYOUTPROBE_H
#define PDFDRAWSPACELAYOUTPROBE_H

#include "pdfwidgetsglobal.h"

#include "pdfcatalog.h"
#include "pdfpage.h"

#include <QList>
#include <QRectF>

namespace pdf
{
class PDFDocument;

/// Read-only access to the Widgets draw-space layout, for the Phase 4 migration
/// oracle and nothing else.
///
/// P4-S6 has to prove that the host-neutral ViewportController lays a document
/// out where the Widgets path lays it out. The arithmetic that answers this lives
/// in PDFDrawSpaceController, which is a plain QObject in a public header but is
/// not exported -- so a test outside this library cannot reach it on Windows,
/// where a shared library exports nothing without the macro.
///
/// This is that reach, deliberately kept to one exported free function over one
/// value type rather than exporting the controller wholesale: no signals, no
/// document ownership, no widget, and nothing a product surface could grow a
/// dependency on. UnitTestsCanvasParity is its only caller.
///
/// Phase 5 deletes this with LoupeLibWidgets. It is migration-only, it is not a
/// product contract, and nothing outside the parity gate may call it.
class LOUPELIBWIDGETSSHARED_EXPORT PDFDrawSpaceLayoutProbe
{
public:
    PDFDrawSpaceLayoutProbe() = delete;

    /// Page rectangles in millimetres, one per page in document order, exactly as
    /// PDFDrawSpaceController laid them out. A page the controller did not lay out
    /// is an empty rectangle rather than a missing entry, so the result indexes by
    /// page and the two sides of a comparison stay aligned.
    static QList<QRectF> layoutPageRectsMM(const PDFDocument* document, PageLayout layout, PageRotation rotation);
};

}   // namespace pdf

#endif   // PDFDRAWSPACELAYOUTPROBE_H
