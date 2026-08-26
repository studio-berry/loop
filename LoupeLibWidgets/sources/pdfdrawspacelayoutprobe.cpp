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

#include "pdfdrawspacelayoutprobe.h"

#include "pdfdocument.h"
#include "pdfdrawspacecontroller.h"

namespace pdf
{

QList<QRectF> PDFDrawSpaceLayoutProbe::layoutPageRectsMM(const PDFDocument* document, PageLayout layout, PageRotation rotation)
{
    QList<QRectF> rects;
    if (!document)
    {
        return rects;
    }

    // The controller is constructed, asked and destroyed here. It is not exposed,
    // not kept, and never given a widget -- the layout is a pure function of the
    // document, the layout mode and the rotation, and that is all this probe is
    // for.
    PDFDrawSpaceController controller(nullptr);
    controller.setPageLayout(layout);
    controller.setPageRotation(rotation);

    // No optional content activity: PDFDrawSpaceController documents nullptr as
    // "no content is suppressed", which is what a layout comparison wants.
    controller.setDocument(PDFModifiedDocument(const_cast<PDFDocument*>(document), nullptr));

    const size_t pageCount = document->getCatalog()->getPageCount();
    rects.reserve(int(pageCount));

    for (size_t index = 0; index < pageCount; ++index)
    {
        const PDFDrawSpaceController::LayoutItem item = controller.getLayoutItemForPage(PDFInteger(index));
        rects.append(item.isValid() ? item.pageRectMM : QRectF());
    }

    return rects;
}

}   // namespace pdf
