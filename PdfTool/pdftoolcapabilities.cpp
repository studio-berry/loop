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

#include "pdftoolcapabilities.h"

#include "pdffixupregistry.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

namespace pdftool
{

namespace
{

QString valueTypeName(PDFToolValueType valueType)
{
    switch (valueType)
    {
        case PDFToolValueType::Boolean: return QStringLiteral("boolean");
        case PDFToolValueType::Integer: return QStringLiteral("integer");
        case PDFToolValueType::Number: return QStringLiteral("number");
        case PDFToolValueType::String: return QStringLiteral("string");
        case PDFToolValueType::Path: return QStringLiteral("path");
        case PDFToolValueType::Enum: return QStringLiteral("enum");
        case PDFToolValueType::Csv: return QStringLiteral("csv");
    }
    return QStringLiteral("string");
}

QJsonArray stringArray(const QStringList& values)
{
    QJsonArray result;
    for (const QString& value : values)
    {
        result.append(value);
    }
    return result;
}

QJsonObject optionToJson(const PDFToolOptionDescriptor& option)
{
    return {
        { QStringLiteral("id"), option.id },
        { QStringLiteral("names"), stringArray(option.names) },
        { QStringLiteral("value_name"), option.valueName },
        { QStringLiteral("value_type"), valueTypeName(option.valueType) },
        { QStringLiteral("allowed_values"), stringArray(option.allowedValues) },
        { QStringLiteral("default_value"), option.defaultValue },
        { QStringLiteral("required"), option.required },
        { QStringLiteral("repeatable"), option.repeatable },
        { QStringLiteral("sensitive"), option.sensitive }
    };
}

QJsonObject positionalToJson(const PDFToolPositionalDescriptor& positional)
{
    return {
        { QStringLiteral("id"), positional.id },
        { QStringLiteral("value_type"), valueTypeName(positional.valueType) },
        { QStringLiteral("required"), positional.required },
        { QStringLiteral("repeatable"), positional.repeatable }
    };
}

QJsonObject commandToJson(const PDFToolCommandDescriptor& command)
{
    QJsonArray options;
    for (const PDFToolOptionDescriptor& option : command.options)
    {
        options.append(optionToJson(option));
    }

    QJsonArray positionals;
    for (const PDFToolPositionalDescriptor& positional : command.positionals)
    {
        positionals.append(positionalToJson(positional));
    }

    return {
        { QStringLiteral("id"), command.id },
        { QStringLiteral("name"), command.name },
        { QStringLiteral("description"), command.description },
        { QStringLiteral("capabilities"), stringArray(command.capabilities) },
        { QStringLiteral("output_formats"), stringArray(command.outputFormats) },
        { QStringLiteral("options"), options },
        { QStringLiteral("positionals"), positionals }
    };
}

QJsonArray buildCapabilities()
{
    QStringList capabilities{
        QStringLiteral("core.pdf.read"),
        QStringLiteral("core.pdf.write"),
        QStringLiteral("pdftool.discovery.v1")
    };
#ifdef LOOP_ENABLE_SENTRY
    capabilities.append(QStringLiteral("telemetry.sentry"));
#endif
    if (PDFToolApplicationStorage::getApplicationByCommand(QStringLiteral("preflight")))
    {
        capabilities.append(QStringLiteral("preflight"));
    }
    if (PDFToolApplicationStorage::getApplicationByCommand(QStringLiteral("ocr")))
    {
        capabilities.append(QStringLiteral("ocr.client"));
    }
    capabilities.sort();
    return stringArray(capabilities);
}

QJsonArray fixupCapabilities()
{
    QList<pdf::PDFFixupCapability> fixups = pdf::getImplementedFixupCapabilities();
    std::sort(fixups.begin(), fixups.end(), [](const auto& left, const auto& right) { return left.id < right.id; });

    QJsonArray result;
    for (const pdf::PDFFixupCapability& fixup : fixups)
    {
        result.append(QJsonObject{
            { QStringLiteral("id"), fixup.id },
            { QStringLiteral("implemented"), fixup.implemented },
            { QStringLiteral("destructive"), fixup.destructive },
            { QStringLiteral("supports_dry_run"), fixup.supportsDryRun },
            { QStringLiteral("supports_report"), fixup.supportsReport }
        });
    }
    return result;
}

QJsonArray schemaCapabilities()
{
    return {
        QJsonObject{{ QStringLiteral("id"), QStringLiteral("loop-preflight-profile") }, { QStringLiteral("version"), 1 }},
        QJsonObject{{ QStringLiteral("id"), QStringLiteral("loop-preflight-report") }, { QStringLiteral("version"), 3 }},
        QJsonObject{{ QStringLiteral("id"), QStringLiteral("pdftool-discovery") }, { QStringLiteral("version"), 1 }},
        QJsonObject{{ QStringLiteral("id"), QStringLiteral("pdftool-envelope") }, { QStringLiteral("version"), 1 }}
    };
}

} // namespace

PDFToolCapabilitiesApplication::PDFToolCapabilitiesApplication() :
    PDFToolAbstractApplication()
{
}

QString PDFToolCapabilitiesApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command: return QStringLiteral("capabilities");
        case Name: return PDFToolTranslationContext::tr("Capabilities");
        case Description: return PDFToolTranslationContext::tr("Describe the commands, options, schemas, fixups, and build capabilities in this PdfTool binary.");
    }
    return QString();
}

PDFToolExitCode PDFToolCapabilitiesApplication::execute(const PDFToolOptions& options)
{
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("The capabilities command only supports JSON output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QList<PDFToolCommandDescriptor> descriptors;
    for (PDFToolAbstractApplication* application : PDFToolApplicationStorage::getApplications())
    {
        descriptors.append(application->describe());
    }
    std::sort(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) { return left.id < right.id; });

    const QString requestedCommand = options.capabilitiesCommand;
    if (!requestedCommand.isEmpty())
    {
        const auto found = std::find_if(descriptors.cbegin(), descriptors.cend(), [&](const auto& descriptor) {
            return descriptor.id == requestedCommand;
        });
        if (found == descriptors.cend())
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("cli.unknown-discovery-command"),
                             PDFToolTranslationContext::tr("Unknown command requested for discovery: '%1'.").arg(requestedCommand),
                             QJsonObject{{ QStringLiteral("command"), requestedCommand }});
            return PDFToolExitCode::InvalidInvocation;
        }
        descriptors = { *found };
    }

    QJsonArray commands;
    for (const PDFToolCommandDescriptor& descriptor : descriptors)
    {
        commands.append(commandToJson(descriptor));
    }

    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{
            { QStringLiteral("discovery_schema_version"), 1 },
            { QStringLiteral("product"), QJsonObject{
                { QStringLiteral("name"), QCoreApplication::applicationName() },
                { QStringLiteral("version"), QCoreApplication::applicationVersion() }
            } },
            { QStringLiteral("output_contract"), QJsonObject{
                { QStringLiteral("schema_version"), 1 },
                { QStringLiteral("console_formats"), QJsonArray{ QStringLiteral("html"), QStringLiteral("json"), QStringLiteral("text"), QStringLiteral("xml") } }
            } },
            { QStringLiteral("build_capabilities"), buildCapabilities() },
            { QStringLiteral("fixups"), fixupCapabilities() },
            { QStringLiteral("schemas"), schemaCapabilities() },
            { QStringLiteral("commands"), commands }
        });
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolCapabilitiesApplication::getOptionsFlags() const
{
    return ConsoleFormat | CapabilityDiscovery;
}

static PDFToolCapabilitiesApplication s_capabilitiesApplication;

} // namespace pdftool
