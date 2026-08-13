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

#ifndef PDFSENTRY_H
#define PDFSENTRY_H

#include "pdfglobal.h"

#include <QString>

namespace pdf
{

/// RAII wrapper for sentry-native. No-op when PDF4QT_ENABLE_SENTRY is off or
/// SENTRY_DSN is `off`/`0`/`disabled`. Windows builds compile in the Loupe DSN.
class PDF4QTLIBCORESHARED_EXPORT PDFSentrySession
{
public:
    explicit PDFSentrySession(const QString& applicationId);
    ~PDFSentrySession();

    PDFSentrySession(const PDFSentrySession&) = delete;
    PDFSentrySession& operator=(const PDFSentrySession&) = delete;

    bool isActive() const { return m_active; }

    static bool isGloballyActive();
    static void captureVerificationEvent();
    static void traceStartup(const QString& applicationId);
    static void flush(int timeoutMilliseconds = 2000);

private:
    bool m_active = false;
};

/// One performance transaction (PdfTool command, GUI session, or startup).
class PDF4QTLIBCORESHARED_EXPORT PDFSentryTransaction
{
public:
    explicit PDFSentryTransaction(const QString& name, const char* operation = "app.command");
    ~PDFSentryTransaction();

    PDFSentryTransaction(const PDFSentryTransaction&) = delete;
    PDFSentryTransaction& operator=(const PDFSentryTransaction&) = delete;

private:
    void* m_transaction = nullptr;
};

}   // namespace pdf

#endif // PDFSENTRY_H
