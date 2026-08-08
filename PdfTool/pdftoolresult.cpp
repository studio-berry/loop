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

#include "pdftoolresult.h"

#include <QCoreApplication>
#include <QJsonArray>

namespace pdftool
{

QJsonObject PDFToolDiagnostic::toJson() const
{
    QString severityName = QStringLiteral("error");
    switch (severity)
    {
        case PDFToolDiagnosticSeverity::Info:
            severityName = QStringLiteral("info");
            break;

        case PDFToolDiagnosticSeverity::Warning:
            severityName = QStringLiteral("warning");
            break;

        case PDFToolDiagnosticSeverity::Error:
            severityName = QStringLiteral("error");
            break;
    }

    QJsonObject object;
    object.insert(QStringLiteral("severity"), severityName);
    object.insert(QStringLiteral("code"), code);
    object.insert(QStringLiteral("message"), message);
    if (!context.isEmpty())
    {
        object.insert(QStringLiteral("context"), context);
    }
    return object;
}

QJsonObject PDFToolOutput::toJson() const
{
    return QJsonObject{
        { QStringLiteral("kind"), kind },
        { QStringLiteral("role"), role },
        { QStringLiteral("path"), path },
        { QStringLiteral("state"), state }
    };
}

PDFToolExecutionContext::PDFToolExecutionContext(QString command) :
    m_command(std::move(command))
{

}

void PDFToolExecutionContext::addDiagnostic(PDFToolDiagnostic diagnostic)
{
    m_diagnostics.push_back(std::move(diagnostic));
}

void PDFToolExecutionContext::addOutput(PDFToolOutput output)
{
    m_outputs.push_back(std::move(output));
}

void PDFToolExecutionContext::setData(QJsonObject data)
{
    m_data = std::move(data);
}

QJsonObject PDFToolExecutionContext::toJson(PDFToolExitCode exitCode) const
{
    QJsonArray diagnostics;
    for (const PDFToolDiagnostic& diagnostic : m_diagnostics)
    {
        diagnostics.append(diagnostic.toJson());
    }

    QJsonArray outputs;
    for (const PDFToolOutput& output : m_outputs)
    {
        outputs.append(output.toJson());
    }

    return QJsonObject{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("command"), m_command },
        { QStringLiteral("version"), QCoreApplication::applicationVersion() },
        { QStringLiteral("status"), pdfToolStatusName(exitCode) },
        { QStringLiteral("exit_code"), static_cast<int>(exitCode) },
        { QStringLiteral("diagnostics"), diagnostics },
        { QStringLiteral("outputs"), outputs },
        { QStringLiteral("data"), m_data }
    };
}

QString pdfToolStatusName(PDFToolExitCode exitCode)
{
    switch (exitCode)
    {
        case PDFToolExitCode::Success:
            return QStringLiteral("success");

        case PDFToolExitCode::Findings:
            return QStringLiteral("findings");

        case PDFToolExitCode::InvalidInvocation:
            return QStringLiteral("invalid-invocation");

        case PDFToolExitCode::InputError:
            return QStringLiteral("input-error");

        case PDFToolExitCode::ProcessingFailure:
            return QStringLiteral("processing-failure");

        case PDFToolExitCode::PartialOutput:
            return QStringLiteral("partial-output");

        case PDFToolExitCode::Cancelled:
            return QStringLiteral("cancelled");

        case PDFToolExitCode::InternalError:
            return QStringLiteral("internal-error");
    }

    return QStringLiteral("internal-error");
}

} // namespace pdftool