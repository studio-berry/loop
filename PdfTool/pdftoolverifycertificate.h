#ifndef PDFTOOLVERIFYCERTIFICATE_H
#define PDFTOOLVERIFYCERTIFICATE_H

#include "pdftoolabstractapplication.h"

namespace pdftool
{

class PDFToolVerifyCertificate final : public PDFToolAbstractApplication
{
public:
    QString getStandardString(StandardString standardString) const override;
    PDFToolExitCode execute(const PDFToolOptions& options) override;
    Options getOptionsFlags() const override;
};

}   // namespace pdftool

#endif   // PDFTOOLVERIFYCERTIFICATE_H
