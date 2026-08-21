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

#ifndef PDFWORKLOADENVELOPE_H
#define PDFWORKLOADENVELOPE_H

#include "pdfglobal.h"

#include <QJsonObject>
#include <QString>

namespace pdf
{

/// Machine identity attached to measured envelopes and PdfTool benchmark JSON.
struct LOUPELIBCORESHARED_EXPORT PDFRunIdentity
{
    QString commit;
    QString compiler;
    QString buildType;
    QString os;
    QString qtVersion;
    QString cpuArchitecture;
    QString gpu;
    QString renderer;
    QString fixtureDigest;
    QString profileVersion;
    QString operationVersion;
    QString productVersion;

    static PDFRunIdentity capture();
    QJsonObject toJson() const;
    static QString digestFile(const QString& path);
    static QString digestBytes(const QByteArray& bytes);
};

/// Measured huge-document envelope.
struct LOUPELIBCORESHARED_EXPORT PDFWorkloadEnvelope
{
    PDFRunIdentity identity;
    QString family;
    QString status = QStringLiteral("complete");
    QString incompleteReason;
    qint64 pageCount = 0;
    qint64 openToFirstViewMs = -1;
    qint64 rssHighWaterBytes = 0;
    qint64 cacheHighWaterBytes = -1;
    qint64 elapsedMs = 0;
    qint64 cancellationLatencyMs = -1;
    qint64 recoveryMs = -1;
    qint64 pressureShedCount = 0;
    bool prefetchShed = false;
    bool interactionSlotHeld = false;

    static qint64 currentRssHighWaterBytes();
    QJsonObject toJson() const;
};

}   // namespace pdf

#endif   // PDFWORKLOADENVELOPE_H
