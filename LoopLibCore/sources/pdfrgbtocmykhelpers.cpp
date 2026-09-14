#include "pdfrgbtocmykhelpers.h"

#include <QCryptographicHash>

namespace pdf
{

bool isRgbColorSpaceName(const QByteArray& name)
{
    return name == QByteArrayLiteral("DeviceRGB") || name == QByteArrayLiteral("RGB");
}

QByteArray targetProfileId(const PDFRgbToCmykSettings& settings)
{
    if (!settings.targetIccId.isEmpty())
    {
        return settings.targetIccId;
    }
    return QCryptographicHash::hash(settings.targetIccData, QCryptographicHash::Sha256);
}

}   // namespace pdf
