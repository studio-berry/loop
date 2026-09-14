#ifndef PDFRGBTOCMYKHELPERS_H
#define PDFRGBTOCMYKHELPERS_H

#include "pdfrgbtocmykfixup.h"

#include <QByteArray>

namespace pdf
{

bool isRgbColorSpaceName(const QByteArray& name);
QByteArray targetProfileId(const PDFRgbToCmykSettings& settings);

}   // namespace pdf

#endif   // PDFRGBTOCMYKHELPERS_H
