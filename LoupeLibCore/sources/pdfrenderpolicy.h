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

#ifndef PDFRENDERPOLICY_H
#define PDFRENDERPOLICY_H

#include <QString>
#include <QStringList>

namespace pdf
{

enum class PDFRenderPurpose
{
    InteractiveCanvas,
    PrintPreview,
    PreflightAnalysis,
    SeparationPreview
};

struct PDFRenderPolicy
{
    PDFRenderPurpose purpose = PDFRenderPurpose::InteractiveCanvas;
    bool requireSeparationAccuracy = false;
    bool requireOverprintAccuracy = false;
    bool allowApproximation = true;

    bool requiresAuthoritativeRenderer() const
    {
        return requireSeparationAccuracy || requireOverprintAccuracy || purpose == PDFRenderPurpose::PreflightAnalysis || purpose == PDFRenderPurpose::SeparationPreview;
    }

    static PDFRenderPolicy forOutputPreview()
    {
        PDFRenderPolicy policy;
        policy.purpose = PDFRenderPurpose::SeparationPreview;
        policy.requireSeparationAccuracy = true;
        policy.requireOverprintAccuracy = true;
        policy.allowApproximation = false;
        return policy;
    }

    static PDFRenderPolicy forPreflightAnalysis()
    {
        PDFRenderPolicy policy;
        policy.purpose = PDFRenderPurpose::PreflightAnalysis;
        policy.requireOverprintAccuracy = true;
        policy.allowApproximation = false;
        return policy;
    }

    static PDFRenderPolicy forSeparationPreview()
    {
        return forOutputPreview();
    }
};

enum class PDFRenderFidelity
{
    ExactSupported,
    SupportedWithFallback,
    Unsupported
};

struct PDFRenderDiagnostics
{
    PDFRenderFidelity fidelity = PDFRenderFidelity::ExactSupported;
    QStringList reasons;

    void record(PDFRenderFidelity newFidelity, const QString& reason)
    {
        if (newFidelity == PDFRenderFidelity::Unsupported || (newFidelity == PDFRenderFidelity::SupportedWithFallback && fidelity == PDFRenderFidelity::ExactSupported))
        {
            fidelity = newFidelity;
        }

        if (!reason.isEmpty() && !reasons.contains(reason))
        {
            reasons.push_back(reason);
        }
    }

    bool isExact() const { return fidelity == PDFRenderFidelity::ExactSupported; }

    /// Records the deliberate limitation of the fast renderer. Callers pass a
    /// cached page-content fact; this function never scans a content stream.
    void recordStandardRendererOverprintApproximation(bool pageContainsOverprint,
                                                      const PDFRenderPolicy& policy)
    {
        if (pageContainsOverprint && policy.allowApproximation)
        {
            record(PDFRenderFidelity::SupportedWithFallback,
                   QStringLiteral("Overprint is approximated by the standard renderer."));
        }
    }
};

}   // namespace pdf

#endif   // PDFRENDERPOLICY_H
