// MIT License
#include "pdfdocumentreader.h"

#include <QFile>

namespace pdf
{

PDFDocument PDFDocumentReader::readFromFile(const QString& fileName)
{
    QFile file(fileName);

    reset();
    if (isOperationCancelled())
    {
        m_result = Result::Cancelled;
        return PDFDocument();
    }

    if (!file.exists())
    {
        m_result = Result::Failed;
        m_errorMessage = tr("File '%1' doesn't exist.").arg(fileName);
        return PDFDocument();
    }

    if (!file.open(QFile::ReadOnly))
    {
        m_result = Result::Failed;
        m_errorMessage = tr("File '%1' cannot be opened for reading. %2")
                             .arg(fileName, file.errorString());
        return PDFDocument();
    }

    return readFromDevice(&file);
}

}   // namespace pdf
