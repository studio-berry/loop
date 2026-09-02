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

#ifndef PDFAPPLICATIONIDENTITY_H
#define PDFAPPLICATIONIDENTITY_H

#include "pdfglobal.h"

#include <QString>

namespace pdf
{

enum class PDFApplicationSurface
{
    LoopEditor,
    PdfTool,
    CodeGenerator,
    Jbig2Viewer,
    PdfExampleGenerator,
    LoopPreflightFixtureGenerator,
    QuickShellSmoke,
    ProductQuickAccessibilitySmoke,
    CanvasBenchmark,
};

struct LOOPLIBCORESHARED_EXPORT PDFApplicationIdentity final
{
    QString productName;
    QString organizationName;
    QString organizationDomain;
    QString applicationName;
    QString displayName;
    QString packageId;
    QString appUserModelId;
    QString version;
};

/// Return the complete identity for one executable or qualification surface.
LOOPLIBCORESHARED_EXPORT PDFApplicationIdentity getApplicationIdentity(PDFApplicationSurface surface);

/// Apply the identity to the current QCoreApplication and, on Windows, to the
/// process AppUserModelID. The caller must invoke this after constructing the
/// application object and before opening QSettings or creating UI objects.
LOOPLIBCORESHARED_EXPORT void initializeApplicationIdentity(PDFApplicationSurface surface);

}   // namespace pdf

#endif // PDFAPPLICATIONIDENTITY_H
