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

#include "overlayframe.h"

#include <algorithm>

namespace pdfinteraction
{

const char* getOverlayLayerName(OverlayLayer layer)
{
    switch (layer)
    {
        case OverlayLayer::PageChrome:
            return "page-chrome";
        case OverlayLayer::Guides:
            return "guides";
        case OverlayLayer::Findings:
            return "findings";
        case OverlayLayer::Hover:
            return "hover";
        case OverlayLayer::Selection:
            return "selection";
        case OverlayLayer::DragHandles:
            return "drag-handles";
        case OverlayLayer::ToolPreview:
            return "tool-preview";
    }

    return "unknown";
}

const char* getOverlayPrimitiveKindName(OverlayPrimitiveKind kind)
{
    switch (kind)
    {
        case OverlayPrimitiveKind::Rectangle:
            return "rectangle";
        case OverlayPrimitiveKind::Polyline:
            return "polyline";
        case OverlayPrimitiveKind::Marker:
            return "marker";
        case OverlayPrimitiveKind::Handle:
            return "handle";
    }

    return "unknown";
}

const char* getOverlaySeverityName(OverlaySeverity severity)
{
    switch (severity)
    {
        case OverlaySeverity::None:
            return "none";
        case OverlaySeverity::Info:
            return "info";
        case OverlaySeverity::Warning:
            return "warning";
        case OverlaySeverity::Error:
            return "error";
    }

    return "unknown";
}

bool overlayPaintsBefore(const OverlayPrimitive& left, const OverlayPrimitive& right)
{
    if (left.layer != right.layer)
    {
        return int(left.layer) < int(right.layer);
    }

    return left.sequence < right.sequence;
}

QList<OverlayPrimitive> OverlayFrame::primitivesForPage(int pageIndex) const
{
    QList<OverlayPrimitive> result;

    for (const OverlayPrimitive& primitive : primitives)
    {
        if (primitive.pageIndex == pageIndex)
        {
            result.push_back(primitive);
        }
    }

    return result;
}

bool OverlayFrame::isOrdered() const
{
    return std::is_sorted(primitives.cbegin(), primitives.cend(), overlayPaintsBefore);
}

}   // namespace pdfinteraction
