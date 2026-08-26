// MIT License

#ifndef PDFTOOLRENDERPAGE_H
#define PDFTOOLRENDERPAGE_H

#include "pdftoolabstractapplication.h"

namespace pdftool
{

class PDFToolRenderPageApplication : public PDFToolAbstractApplication
{
public:
    QString getStandardString(StandardString standardString) const override;
    PDFToolExitCode execute(const PDFToolOptions& options) override;
    Options getOptionsFlags() const override;
};

}   // namespace pdftool

#endif   // PDFTOOLRENDERPAGE_H
