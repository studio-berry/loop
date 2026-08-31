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

#ifndef RENDERPRESENTATIONPOLICY_H
#define RENDERPRESENTATIONPOLICY_H

#include "interactionglobal.h"

#include "pdfrenderer.h"

#include <QString>

namespace pdfinteraction
{

/// The single render/presentation policy shared by page pixels and overlays.
/// Consumers may read it, but only the owning presentation session should
/// replace it through PageSurfaceCoordinator::setRenderSettings().
struct RenderPresentationPolicy
{
    pdf::PDFRenderer::Features features = pdf::PDFRenderer::getDefaultFeatures();

    /// Identity of the colour-managed output path in force. Opaque to the
    /// interaction layer; it changes whenever the produced pixels would.
    QString colorOutputIdentity;
};

}   // namespace pdfinteraction

#endif   // RENDERPRESENTATIONPOLICY_H
