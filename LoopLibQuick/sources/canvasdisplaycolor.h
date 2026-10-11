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


#ifndef CANVASDISPLAYCOLOR_H
#define CANVASDISPLAYCOLOR_H

#include "loopquickglobal.h"

#include <QColor>
#include <QImage>
#include <QString>

#include <optional>

namespace pdfquick
{

/// The five display colour modes of issue #235, plus the identity.
///
/// A display colour mode is a presentation filter over pixels the render path
/// already produced. It is deliberately not a render request: the mode never
/// reaches a PageSurfaceKey, the page cache, `PDFRenderer::Features` or
/// `PDFRenderPolicy`, so no surface can be mistaken for the pixels a production
/// render would have produced, and switching modes re-renders nothing (issue
/// #235 display half; the production half -- bitonal document creation, bleed
/// fixup -- belongs to PR 3 and is not here).
///
/// The transform itself is Core's own `pdf::PDFColorConvertor`, the same
/// converter `PDFRenderer::applyFeaturesToColorConvertor` configures for the
/// ColorAdjust_* render features, so "invert" means one thing in this repository
/// rather than two.
enum class CanvasDisplayColorMode
{
    Normal,
    Invert,
    Grayscale,
    HighContrast,
    Bitonal,
    Custom
};

/// The vocabulary name of a mode, as `actionColor*` commands and QML use it.
LOOPLIBQUICK_EXPORT QString getCanvasDisplayColorModeName(CanvasDisplayColorMode mode);

/// Parses a name once at a boundary. An unknown name returns nothing rather
/// than being coerced into a default.
LOOPLIBQUICK_EXPORT std::optional<CanvasDisplayColorMode> canvasDisplayColorModeFromName(const QString& name);

/// The shell action id that selects a mode, for the handler the host binds.
LOOPLIBQUICK_EXPORT QString getCanvasDisplayColorModeActionId(CanvasDisplayColorMode mode);

/// Everything the transform reads. The custom colours and the bitonal threshold
/// are settings, not modes, so they stay valid while the mode changes.
struct CanvasDisplayColorSettings
{
    CanvasDisplayColorMode mode = CanvasDisplayColorMode::Normal;
    QColor customForeground = Qt::green;
    QColor customBackground = Qt::black;
    int bitonalThreshold = 128;
    float highContrastSlopeFactor = 10.0F;

    bool isActive() const noexcept { return mode != CanvasDisplayColorMode::Normal; }

    bool operator==(const CanvasDisplayColorSettings&) const = default;
};

/// The display transform. Pure: the input image is not modified and a Normal
/// setting returns it unchanged.
LOOPLIBQUICK_EXPORT QImage applyCanvasDisplayColorMode(const QImage& image, const CanvasDisplayColorSettings& settings);

}   // namespace pdfquick

#endif   // CANVASDISPLAYCOLOR_H
