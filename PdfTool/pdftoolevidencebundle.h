#ifndef PDFTOOLEVIDENCEBUNDLE_H
#define PDFTOOLEVIDENCEBUNDLE_H

#include "pdftoolabstractapplication.h"

namespace pdftool
{

/// Exports the portable proof-of-preflight and sign-off evidence bundle for one
/// document revision.
class PDFToolExportEvidenceBundle final : public PDFToolAbstractApplication
{
public:
    QString getStandardString(StandardString standardString) const override;
    PDFToolExitCode execute(const PDFToolOptions& options) override;
    Options getOptionsFlags() const override;
};

/// Verifies a portable proof-of-preflight and sign-off evidence bundle offline.
class PDFToolVerifyEvidenceBundle final : public PDFToolAbstractApplication
{
public:
    QString getStandardString(StandardString standardString) const override;
    PDFToolExitCode execute(const PDFToolOptions& options) override;
    Options getOptionsFlags() const override;
};

}   // namespace pdftool

#endif   // PDFTOOLEVIDENCEBUNDLE_H
