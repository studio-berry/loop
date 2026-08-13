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

#ifndef PDFSAFEFILEWRITER_H
#define PDFSAFEFILEWRITER_H

#include "pdfglobal.h"
#include "pdfutils.h"

#include <QString>
#include <QStringList>
#include <QList>

#include <functional>

class QIODevice;

namespace pdf
{

struct PDF4QTLIBCORESHARED_EXPORT PDFOutputConflict
{
    QString path;
    QString code;
};

/// Shared atomic, collision-safe output helper (MIC-310). Writes through QSaveFile
/// (temp write + commit/rename), so an existing file is never destroyed before the
/// replacement bytes are durable. PDF documents are written via PDFDocumentWriter
/// with safeWrite=true; this helper covers everything else (attachments, images,
/// XML/JSON exports, plain payloads).
class PDF4QTLIBCORESHARED_EXPORT PDFSafeFileWriter
{
    Q_DECLARE_TR_FUNCTIONS(pdf::PDFSafeFileWriter)

public:
    enum class OverwritePolicy
    {
        Fail,       ///< Reject the write if the target file already exists
        Overwrite   ///< Replace the target atomically, only after new bytes are durable
    };

    /// Atomically writes \p data through QSaveFile (temp + commit/rename).
    /// \param fileName Target file name
    /// \param data Data to write
    /// \param policy Overwrite policy
    static PDFOperationResult writeData(const QString& fileName, const QByteArray& data,
                                        OverwritePolicy policy);

    /// Atomic write driven by a caller-supplied producer. The producer receives the
    /// open QSaveFile and must return true only when all bytes were written. On
    /// failure the temporary file is dropped and the previous target is left intact.
    /// \param fileName Target file name
    /// \param producer Producer writing into the device
    /// \param policy Overwrite policy
    static PDFOperationResult writeDevice(const QString& fileName,
                                          const std::function<bool(QIODevice*)>& producer,
                                          OverwritePolicy policy);

    /// Finds duplicate planned paths and, optionally, destinations that already exist.
    /// Paths are compared as absolute, cleaned paths using the host filesystem's
    /// case sensitivity rules.
    static QList<PDFOutputConflict> findOutputConflicts(const QStringList& fileNames,
                                                        bool rejectExisting);

    /// Returns \p fileName when free, otherwise the first free "base (n).ext" variant.
    /// Used for names that collide within a single run; a file that already exists on
    /// disk across runs is governed by --overwrite semantics instead.
    static QString makeUniqueFileName(const QString& fileName);
};

}   // namespace pdf

#endif // PDFSAFEFILEWRITER_H
