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

#include "pdftoolabstractapplication.h"
#include "pdftoolcancel.h"
#include "pdftoolresult.h"
#include "pdfconstants.h"
#include "pdfsentry.h"

#include <QDir>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStringList>
#include <QStringConverter>

#include <csignal>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace
{

QString executableDirectory(const char* argv0)
{
#if defined(Q_OS_WIN)
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        return QFileInfo(QString::fromWCharArray(modulePath, int(length))).absolutePath();
    }
#endif

    const QFileInfo argvInfo(QString::fromLocal8Bit(argv0));
    if (argvInfo.isAbsolute())
    {
        return argvInfo.absolutePath();
    }

    // Bare argv[0] (PATH lookup) is not the process CWD — fall back to CWD only
    // after absolute-path resolution fails so plugin dirs still work for local runs.
    return QDir::currentPath();
}

/// Pre-scan the raw command line for a JSON console-format request. This must be
/// done before parsing so malformed command lines still produce a valid JSON
/// error envelope when the caller asked for JSON.
bool commandLineRequestsJson(const QStringList& arguments)
{
    for (qsizetype i = 0; i < arguments.size(); ++i)
    {
        if (arguments[i] == QStringLiteral("--console-format") &&
            i + 1 < arguments.size() &&
            arguments[i + 1] == QStringLiteral("json"))
        {
            return true;
        }

        if (arguments[i] == QStringLiteral("--console-format=json"))
        {
            return true;
        }
    }

    return false;
}

bool commandLineSpecifiesConsoleFormat(const QStringList& arguments)
{
    for (const QString& argument : arguments)
    {
        if (argument == QStringLiteral("--console-format") ||
            argument.startsWith(QStringLiteral("--console-format=")))
        {
            return true;
        }
    }

    return false;
}

QString requestedCommand(const QStringList& arguments)
{
    for (qsizetype i = 1; i < arguments.size(); ++i)
    {
        const QString& argument = arguments[i];
        if (argument == QStringLiteral("--console-format") ||
            argument == QStringLiteral("--text-codec"))
        {
            ++i;
            continue;
        }

        if (!argument.startsWith('-'))
        {
            return argument;
        }
    }

    return QString();
}

/// Writes the result envelope to stdout (compact JSON, single document) and
/// returns the process exit code that must agree with its exit_code field.
int writeJsonEnvelope(const pdftool::PDFToolExecutionContext& context, pdftool::PDFToolExitCode exitCode)
{
    const QByteArray json = QJsonDocument(context.toJson(exitCode)).toJson(QJsonDocument::Compact);
    pdftool::PDFConsole::writeData(json);
    pdftool::PDFConsole::writeData(QByteArray("\n"));

    return static_cast<int>(exitCode);
}

int writeInvocationError(const pdftool::PDFToolExecutionContext& context,
                         pdftool::PDFToolExitCode exitCode,
                         bool wantsJson,
                         const QString& message)
{
    if (wantsJson)
    {
        return writeJsonEnvelope(context, exitCode);
    }

    if (!message.isEmpty())
    {
        pdftool::PDFConsole::writeError(message, QStringConverter::Utf8);
    }

    return static_cast<int>(exitCode);
}

void handleTerminationSignal(int)
{
    pdftool::cancelRequested().store(true, std::memory_order_release);
}

} // namespace

int main(int argc, char *argv[])
{
    // Prefer offscreen when requested; ensure the exe dir is searched for plugins
    // (platforms/qoffscreen.dll) before QGuiApplication constructs the QPA plugin.
    QCoreApplication::setLibraryPaths(QStringList{ executableDirectory(argv[0]) }
                                      + QCoreApplication::libraryPaths());

    QGuiApplication a(argc, argv);
    QCoreApplication::setOrganizationName("MelkaJ");
    QCoreApplication::setApplicationName("PdfTool");
    QCoreApplication::setApplicationVersion(pdf::PDF_LIBRARY_VERSION);

    const pdf::PDFSentrySession sentrySession(QStringLiteral("pdftool"));

    const QStringList arguments = QCoreApplication::arguments();
    const bool wantsJson = commandLineRequestsJson(arguments);

    // Extract the requested command without terminating on unknown options so
    // invalid invocations can be reported through the result contract.
    QCommandLineParser parser;
    parser.setApplicationDescription("PdfTool - work with pdf documents via command line");
    parser.addPositionalArgument("command", "Command to execute.");
    parser.parse(arguments);

    const QString command = requestedCommand(arguments);

    pdftool::PDFToolAbstractApplication* application = pdftool::PDFToolApplicationStorage::getApplicationByCommand(command);

    // An unknown command is an invalid invocation, not a silent fallback to help.
    if (!application && !command.isEmpty())
    {
        pdftool::PDFToolExecutionContext context(command);
        if (wantsJson)
        {
            pdftool::PDFConsole::setDiagnosticSink(&context);
        }
        context.addDiagnostic({
            pdftool::PDFToolDiagnosticSeverity::Error,
            QStringLiteral("cli.unknown-command"),
            pdftool::PDFToolTranslationContext::tr("Unknown command '%1'.").arg(command),
            {}
        });

        if (wantsJson)
        {
            return writeJsonEnvelope(context, pdftool::PDFToolExitCode::InvalidInvocation);
        }

        // Human mode: report the error, still show help text, but exit nonzero.
        pdftool::PDFConsole::writeError(
            pdftool::PDFToolTranslationContext::tr("Unknown command '%1'.").arg(command),
            QStringConverter::Utf8);

        pdftool::PDFToolAbstractApplication* helpApplication = pdftool::PDFToolApplicationStorage::getDefaultApplication();
        pdftool::PDFToolExecutionContext helpContext(QStringLiteral("help"));
        QCommandLineParser helpParser;
        helpApplication->initializeCommandLineParser(&helpParser);
        helpParser.addHelpOption();
        helpParser.addVersionOption();
        helpParser.parse(QStringList());
        helpApplication->execute(helpApplication->getOptions(&helpParser, &helpContext));

        return static_cast<int>(pdftool::PDFToolExitCode::InvalidInvocation);
    }

    if (!application)
    {
        application = pdftool::PDFToolApplicationStorage::getDefaultApplication();
    }
    else
    {
        parser.clearPositionalArguments();
    }

    QStringList commandArguments = arguments;
    if (!command.isEmpty())
    {
        commandArguments.removeOne(command);
    }

    const QString displayCommand = application->getStandardString(pdftool::PDFToolAbstractApplication::Command);
    pdftool::PDFToolExecutionContext context(displayCommand);
    if (wantsJson ||
        ((displayCommand == QStringLiteral("preflight") || displayCommand == QStringLiteral("ocr")) &&
         !commandLineSpecifiesConsoleFormat(arguments)))
    {
        pdftool::PDFConsole::setDiagnosticSink(&context);
    }

    application->initializeCommandLineParser(&parser);

    parser.addHelpOption();
    parser.addVersionOption();
    if (!parser.parse(commandArguments))
    {
        context.addDiagnostic({
            pdftool::PDFToolDiagnosticSeverity::Error,
            QStringLiteral("cli.invalid-arguments"),
            parser.errorText(),
            {}
        });

        return writeInvocationError(context,
                                    pdftool::PDFToolExitCode::InvalidInvocation,
                                    wantsJson,
                                    parser.errorText());
    }

    if (!wantsJson && parser.isSet("help"))
    {
        parser.showHelp();
    }

    if (!wantsJson && parser.isSet("version"))
    {
        parser.showVersion();
    }

    pdftool::resetCancelRequested();
    std::signal(SIGINT, handleTerminationSignal);
#ifndef Q_OS_WIN
    std::signal(SIGTERM, handleTerminationSignal);
#endif

    const QString sentryCommand = command.isEmpty() ? QStringLiteral("help") : command;
    const pdf::PDFSentryTransaction sentryTransaction(sentryCommand, "pdftool.command");

    const pdftool::PDFToolOptions options = application->getOptions(&parser, &context);
    const pdftool::PDFToolExitCode exitCode = application->execute(options);

    if (options.outputStyle == pdftool::PDFOutputFormatter::Style::Json)
    {
        return writeJsonEnvelope(context, exitCode);
    }

    return static_cast<int>(exitCode);
}
