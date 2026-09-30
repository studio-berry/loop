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

#ifndef PDFWORKERPROTOCOL_H
#define PDFWORKERPROTOCOL_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace pdftool::worker
{

/// Wire protocol version for PdfTool supervisor ↔ loop-pdf-worker IPC.
/// Newline-delimited JSON; allowlist ops only (not a general RPC).
inline constexpr int PROTOCOL_VERSION = 1;

inline constexpr qint64 DEFAULT_RSS_LIMIT_BYTES = qint64(768) * 1024 * 1024;
inline constexpr qint64 DEFAULT_CPU_SECONDS = 120;

inline QStringList allowedOperations()
{
    return {
        QStringLiteral("ping"),
        QStringLiteral("open"),
        QStringLiteral("preflight"),
        QStringLiteral("cancel"),
    };
}

inline bool isAllowedOperation(const QString& operation)
{
    return allowedOperations().contains(operation);
}

inline QJsonObject makeErrorResponse(const QString& id,
                                     const QString& op,
                                     const QString& status,
                                     const QString& code,
                                     const QString& reason)
{
    return QJsonObject{
        { QStringLiteral("v"), PROTOCOL_VERSION },
        { QStringLiteral("id"), id },
        { QStringLiteral("op"), op },
        { QStringLiteral("ok"), false },
        { QStringLiteral("status"), status },
        { QStringLiteral("code"), code },
        { QStringLiteral("reason"), reason },
    };
}

inline QJsonObject makeOkResponse(const QString& id, const QString& op, const QString& status, QJsonObject data = {})
{
    QJsonObject response{
        { QStringLiteral("v"), PROTOCOL_VERSION },
        { QStringLiteral("id"), id },
        { QStringLiteral("op"), op },
        { QStringLiteral("ok"), true },
        { QStringLiteral("status"), status },
    };
    for (auto it = data.begin(); it != data.end(); ++it)
    {
        response.insert(it.key(), it.value());
    }
    return response;
}

}   // namespace pdftool::worker

#endif   // PDFWORKERPROTOCOL_H
