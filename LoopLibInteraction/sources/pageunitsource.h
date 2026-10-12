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

#ifndef PAGEUNITSOURCE_H
#define PAGEUNITSOURCE_H

#include "interactionglobal.h"

#include <QtGlobal>

namespace pdfinteraction
{

/// The scale of a page's user space.
///
/// A measurement is only a production measurement once the page's own /UserUnit
/// is applied: without it a 100-unit line on a /UserUnit 2 page reads half its
/// real length. The interaction layer asks for the scale here instead of
/// reading a document, and a page whose scale cannot be resolved is refused
/// rather than measured at a guessed 1.0.
class IPageUnitSource
{
public:
    virtual ~IPageUnitSource() = default;

    /// Millimetres one page-space point spans, including the page's /UserUnit.
    /// Returns false when the page cannot be resolved.
    virtual bool pagePointToMM(int pageIndex, qreal* factor) const = 0;
};

}   // namespace pdfinteraction

#endif   // PAGEUNITSOURCE_H
