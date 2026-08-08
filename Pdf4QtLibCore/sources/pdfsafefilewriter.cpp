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

#include "pdfsafefilewriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "pdfdbgheap.h"

namespace pdf
{

PDFOperationResult PDFSafeFileWriter::writeData(const QString& fileName, const QByteArray& data,
                                                OverwritePolicy policy)
{
    return writeDevice(fileName, [&data](QIODevice* device) -> bool
    {
        // A short write (disk full, quota) must not be reported as success — that
        // leaves a silently truncated file where a valid output should be.
        const qint64 written = device->write(data);
        return written == data.size();
    }, policy);
}

PDFOperationResult PDFSafeFileWriter::writeDevice(const QString& fileName,
                                                  const std::function<bool(QIODevice*)>& producer,
                                                  OverwritePolicy policy)
{
    const bool isFileExists = QFile::exists(fileName);
    if (isFileExists && policy == OverwritePolicy::Fail)
    {
        return tr("File '%1' already exists.").arg(fileName);
    }

    if (!producer)
    {
        return tr("No writer was provided for file '%1'.").arg(fileName);
    }

    QSaveFile file(fileName);
    file.setDirectWriteFallback(true);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return tr("File '%1' can't be opened for writing. %2").arg(fileName, file.errorString());
    }

    if (!producer(&file))
    {
        file.cancelWriting();
        return tr("File '%1' can't be written. %2").arg(fileName, file.errorString());
    }

    if (!file.commit())
    {
        return tr("File '%1' can't be committed. %2").arg(fileName, file.errorString());
    }

    return true;
}

QString PDFSafeFileWriter::makeUniqueFileName(const QString& fileName)
{
    if (fileName.isEmpty() || !QFile::exists(fileName))
    {
        return fileName;
    }

    const QFileInfo info(fileName);
    const QString baseName = info.completeBaseName();
    const QString suffix = info.suffix();
    const QString directory = info.absolutePath();

    // Bounded probe; the "base (n).ext" cascade frees the path quickly in practice.
    for (qint64 n = 1; n < 100000; ++n)
    {
        QString candidate;
        if (suffix.isEmpty())
        {
            candidate = QDir(directory).filePath(QStringLiteral("%1 (%2)").arg(baseName).arg(n));
        }
        else
        {
            candidate = QDir(directory).filePath(QStringLiteral("%1 (%2).%3").arg(baseName).arg(n).arg(suffix));
        }

        if (!QFile::exists(candidate))
        {
            return candidate;
        }
    }

    return fileName;
}

}   // namespace pdf