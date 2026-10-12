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


#include "canvasdisplaycolor.h"

#include "pdfcolorconvertor.h"

namespace pdfquick
{

namespace
{

pdf::PDFColorConvertor::Mode convertorMode(CanvasDisplayColorMode mode)
{
    switch (mode)
    {
        case CanvasDisplayColorMode::Invert:
            return pdf::PDFColorConvertor::Mode::InvertedColors;
        case CanvasDisplayColorMode::Grayscale:
            return pdf::PDFColorConvertor::Mode::Grayscale;
        case CanvasDisplayColorMode::HighContrast:
            return pdf::PDFColorConvertor::Mode::HighContrast;
        case CanvasDisplayColorMode::Bitonal:
            return pdf::PDFColorConvertor::Mode::Bitonal;
        case CanvasDisplayColorMode::Custom:
            return pdf::PDFColorConvertor::Mode::CustomColors;
        case CanvasDisplayColorMode::Normal:
            break;
    }
    return pdf::PDFColorConvertor::Mode::Normal;
}

}   // namespace

QString getCanvasDisplayColorModeName(CanvasDisplayColorMode mode)
{
    switch (mode)
    {
        case CanvasDisplayColorMode::Normal:
            return QStringLiteral("normal");
        case CanvasDisplayColorMode::Invert:
            return QStringLiteral("invert");
        case CanvasDisplayColorMode::Grayscale:
            return QStringLiteral("grayscale");
        case CanvasDisplayColorMode::HighContrast:
            return QStringLiteral("high-contrast");
        case CanvasDisplayColorMode::Bitonal:
            return QStringLiteral("bitonal");
        case CanvasDisplayColorMode::Custom:
            return QStringLiteral("custom");
    }
    return QStringLiteral("normal");
}

std::optional<CanvasDisplayColorMode> canvasDisplayColorModeFromName(const QString& name)
{
    const QString normalized = name.trimmed().toLower();

    if (normalized == QLatin1String("normal"))
    {
        return CanvasDisplayColorMode::Normal;
    }
    if (normalized == QLatin1String("invert"))
    {
        return CanvasDisplayColorMode::Invert;
    }
    if (normalized == QLatin1String("grayscale"))
    {
        return CanvasDisplayColorMode::Grayscale;
    }
    if (normalized == QLatin1String("high-contrast"))
    {
        return CanvasDisplayColorMode::HighContrast;
    }
    if (normalized == QLatin1String("bitonal"))
    {
        return CanvasDisplayColorMode::Bitonal;
    }
    if (normalized == QLatin1String("custom"))
    {
        return CanvasDisplayColorMode::Custom;
    }
    return std::nullopt;
}

QString getCanvasDisplayColorModeActionId(CanvasDisplayColorMode mode)
{
    switch (mode)
    {
        case CanvasDisplayColorMode::Normal:
            return QString();
        case CanvasDisplayColorMode::Invert:
            return QStringLiteral("actionColorInvert");
        case CanvasDisplayColorMode::Grayscale:
            return QStringLiteral("actionColorGrayscale");
        case CanvasDisplayColorMode::HighContrast:
            return QStringLiteral("actionColorHighContrast");
        case CanvasDisplayColorMode::Bitonal:
            return QStringLiteral("actionColorBitonal");
        case CanvasDisplayColorMode::Custom:
            return QStringLiteral("actionColorCustom");
    }
    return QString();
}

QImage applyCanvasDisplayColorMode(const QImage& image, const CanvasDisplayColorSettings& settings)
{
    if (!settings.isActive() || image.isNull())
    {
        return image;
    }

    pdf::PDFColorConvertor convertor;
    convertor.setMode(convertorMode(settings.mode));
    convertor.setBitonalThreshold(settings.bitonalThreshold);
    convertor.setHighContrastBrightnessFactor(settings.highContrastSlopeFactor);
    convertor.setForegroundColor(settings.customForeground);
    convertor.setBackgroundColor(settings.customBackground);
    return convertor.convert(image);
}

}   // namespace pdfquick
