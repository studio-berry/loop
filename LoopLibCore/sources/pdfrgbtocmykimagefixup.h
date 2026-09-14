#ifndef PDFRGBTOCMYKIMAGEFIXUP_H
#define PDFRGBTOCMYKIMAGEFIXUP_H

#include "pdfrgbtocmykfixup.h"

#include "pdfdocumentbuilder.h"
#include "pdfobject.h"

namespace pdf
{

void scanImageResources(const PDFObject& resourcesObject,
                        const PDFDocument* document,
                        PDFInteger pageIndex,
                        PDFRgbToCmykReport* report);

PDFObjectReference addIccProfileObject(PDFDocumentBuilder* builder, const PDFRgbToCmykSettings& settings);

PDFOperationResult convertRgbImages(const PDFObject& resourcesObject,
                                    const PDFDocument* document,
                                    PDFDocumentBuilder* builder,
                                    const PDFRgbToCmykSettings& settings,
                                    const PDFCMS* cms,
                                    PDFObjectReference profileReference,
                                    PDFRgbToCmykReport* report);

}   // namespace pdf

#endif   // PDFRGBTOCMYKIMAGEFIXUP_H
