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

#include "pdfapplicationidentity.h"

#include "pdfconstants.h"

#include <QCoreApplication>

#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

#ifndef LOOP_PRODUCT_NAME
#define LOOP_PRODUCT_NAME "Loop"
#endif

#ifndef LOOP_ORGANIZATION_NAME
#define LOOP_ORGANIZATION_NAME "Loop"
#endif

#ifndef LOOP_ORGANIZATION_DOMAIN
#define LOOP_ORGANIZATION_DOMAIN "io.github.mberrys"
#endif

#ifndef LOOP_PACKAGE_ID
#define LOOP_PACKAGE_ID "io.github.mberrys.Loop-pdf"
#endif

namespace pdf
{

namespace
{

struct SurfaceNames
{
    const char* applicationName;
    const char* displayName;
};

SurfaceNames namesForSurface(PDFApplicationSurface surface)
{
    switch (surface)
    {
        case PDFApplicationSurface::LoopEditor:
            return { "LoopEditor", "Loop" };
        case PDFApplicationSurface::PdfTool:
            return { "PdfTool", "Loop PdfTool" };
        case PDFApplicationSurface::CodeGenerator:
            return { "CodeGenerator", "Loop Code Generator" };
        case PDFApplicationSurface::Jbig2Viewer:
            return { "JBIG2Viewer", "Loop JBIG2 Viewer" };
        case PDFApplicationSurface::PdfExampleGenerator:
            return { "PdfExampleGenerator", "Loop PDF Example Generator" };
        case PDFApplicationSurface::LoopPreflightFixtureGenerator:
            return { "LoopGenerateFixtures", "Loop Fixture Generator" };
        case PDFApplicationSurface::QuickShellSmoke:
            return { "QuickShellSmoke", "Loop Quick Shell Smoke" };
        case PDFApplicationSurface::ProductQuickAccessibilitySmoke:
            return { "ProductQuickAccessibilitySmoke", "Loop Accessibility Smoke" };
        case PDFApplicationSurface::CanvasBenchmark:
            return { "CanvasBenchmark", "Loop Canvas Benchmark" };
    }

    return { "Loop", "Loop" };
}

}   // namespace

PDFApplicationIdentity getApplicationIdentity(PDFApplicationSurface surface)
{
    const SurfaceNames names = namesForSurface(surface);

    PDFApplicationIdentity identity;
    identity.productName = QStringLiteral(LOOP_PRODUCT_NAME);
    identity.organizationName = QStringLiteral(LOOP_ORGANIZATION_NAME);
    identity.organizationDomain = QStringLiteral(LOOP_ORGANIZATION_DOMAIN);
    identity.applicationName = QString::fromLatin1(names.applicationName);
    identity.displayName = QString::fromLatin1(names.displayName);
    identity.packageId = QStringLiteral(LOOP_PACKAGE_ID);
    identity.appUserModelId = identity.packageId + QLatin1Char('.') + identity.applicationName;
    identity.version = QStringLiteral(LOOP_PROJECT_VERSION);
    return identity;
}

void initializeApplicationIdentity(PDFApplicationSurface surface)
{
    if (!QCoreApplication::instance())
    {
        return;
    }

    const PDFApplicationIdentity identity = getApplicationIdentity(surface);
    QCoreApplication::setOrganizationName(identity.organizationName);
    QCoreApplication::setOrganizationDomain(identity.organizationDomain);
    QCoreApplication::setApplicationName(identity.applicationName);
    QCoreApplication::setApplicationDisplayName(identity.displayName);
    QCoreApplication::setApplicationVersion(identity.version);

#ifdef Q_OS_WIN
    const std::wstring appUserModelId = identity.appUserModelId.toStdWString();
    SetCurrentProcessExplicitAppUserModelID(appUserModelId.c_str());
#endif
}

}   // namespace pdf
