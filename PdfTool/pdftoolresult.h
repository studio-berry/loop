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

#ifndef PDFTOOLRESULT_H
#define PDFTOOLRESULT_H

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace pdftool
{

enum class PDFToolExitCode : int
{
    Success = 0,
    Findings = 1,
    InvalidInvocation = 2,
    InputError = 3,
    ProcessingFailure = 4,
    PartialOutput = 5,
    Cancelled = 6,
    InternalError = 7
};

enum class PDFToolDiagnosticSeverity
{
    Info,
    Warning,
    Error
};

struct PDFToolDiagnostic
{
    PDFToolDiagnosticSeverity severity = PDFToolDiagnosticSeverity::Error;
    QString code;
    QString message;
    QJsonObject context;

    QJsonObject toJson() const;
};

struct PDFToolOutput
{
    QString kind;
    QString role;
    QString path;
    QString state;

    QJsonObject toJson() const;
};

class PDFToolExecutionContext
{
public:
    explicit PDFToolExecutionContext(QString command);

    void addDiagnostic(PDFToolDiagnostic diagnostic);
    void addOutput(PDFToolOutput output);
    void setData(QJsonObject data);

    QJsonObject toJson(PDFToolExitCode exitCode) const;

private:
    QString m_command;
    QVector<PDFToolDiagnostic> m_diagnostics;
    QVector<PDFToolOutput> m_outputs;
    QJsonObject m_data;
};

QString pdfToolStatusName(PDFToolExitCode exitCode);

} // namespace pdftool

#endif // PDFTOOLRESULT_H