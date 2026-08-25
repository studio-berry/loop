// MIT License
#ifndef PDFTOOLSTRUCTUREDOUTPUT_H
#define PDFTOOLSTRUCTUREDOUTPUT_H

#include "pdfoutputformatter.h"

#include <QJsonObject>

namespace pdftool
{

QString formatStructuredObject(const QJsonObject& object,
                               PDFOutputFormatter::Style style,
                               const QString& rootName);

}   // namespace pdftool

#endif   // PDFTOOLSTRUCTUREDOUTPUT_H
