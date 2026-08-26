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

#include "commandcatalog.h"
#include "commandcatalogresource_p.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaType>

#include <algorithm>
#include <utility>

namespace pdfinteraction
{

namespace
{

constexpr const char* ContractResourcePath = ":/loupe/loupe-shell-actions.json";

#ifdef Q_OS_WIN
constexpr bool IsWindowsHost = true;
#else
constexpr bool IsWindowsHost = false;
#endif

/// Resolves the platform variant at load time so a descriptor carries one
/// shortcut rather than a branch a presentation host has to re-evaluate.
CommandShortcut readShortcut(const QJsonObject& shortcutObject)
{
    QJsonObject effective = shortcutObject;
    if constexpr (IsWindowsHost)
    {
        const QJsonValue windows = shortcutObject.value(QStringLiteral("windows"));
        if (windows.isObject())
        {
            effective = windows.toObject();
        }
    }

    CommandShortcut shortcut;
    shortcut.standardKey = effective.value(QStringLiteral("standard_key")).toString();
    shortcut.sequence = effective.value(QStringLiteral("sequence")).toString();
    return shortcut;
}

}   // namespace

CommandCatalog::CommandCatalog(QObject* parent) :
    QObject(parent)
{
    ensureCommandCatalogResource();

    QFile file(QString::fromLatin1(ContractResourcePath));
    if (!file.open(QIODevice::ReadOnly))
    {
        // A static library that silently loses its resource would present an
        // empty command set as a working one. Say so instead.
        m_loadError = QStringLiteral("command-catalog/resource-unavailable");
        return;
    }

    load(file.readAll());
}

CommandCatalog::CommandCatalog(const QByteArray& contract, QObject* parent) :
    QObject(parent)
{
    load(contract);
}

CommandCatalog::~CommandCatalog() = default;

CommandCatalog* CommandCatalog::fromContract(const QByteArray& contract, QObject* parent)
{
    return new CommandCatalog(contract, parent);
}

void CommandCatalog::load(const QByteArray& contract)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(contract, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        m_loadError = QStringLiteral("command-catalog/malformed-contract");
        return;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != 1)
    {
        m_loadError = QStringLiteral("command-catalog/unsupported-schema-version");
        return;
    }

    const QJsonValue actionsValue = root.value(QStringLiteral("actions"));
    if (!actionsValue.isArray())
    {
        m_loadError = QStringLiteral("command-catalog/missing-actions");
        return;
    }

    const QJsonArray actions = actionsValue.toArray();
    QList<CommandDescriptor> descriptors;
    QHash<CommandId, qsizetype> indexById;
    descriptors.reserve(actions.size());

    for (const QJsonValue& actionValue : actions)
    {
        if (!actionValue.isObject())
        {
            m_loadError = QStringLiteral("command-catalog/malformed-action");
            return;
        }

        const QJsonObject action = actionValue.toObject();
        const QJsonValue commandValue = action.value(QStringLiteral("command"));

        CommandDescriptor descriptor;
        descriptor.id = action.value(QStringLiteral("id")).toString();
        descriptor.disposition = action.value(QStringLiteral("disposition")).toString();
        descriptor.target = action.value(QStringLiteral("target")).toString();

        if (descriptor.id.isEmpty() || !commandValue.isObject())
        {
            m_loadError = QStringLiteral("command-catalog/malformed-action");
            return;
        }

        if (indexById.contains(descriptor.id))
        {
            m_loadError = QStringLiteral("command-catalog/duplicate-command");
            return;
        }

        const QJsonObject command = commandValue.toObject();
        descriptor.labelKey = command.value(QStringLiteral("label_key")).toString();
        if (descriptor.labelKey.isEmpty())
        {
            m_loadError = QStringLiteral("command-catalog/missing-label-key");
            return;
        }

        const QJsonValue shortcutValue = command.value(QStringLiteral("shortcut"));
        if (shortcutValue.isObject())
        {
            descriptor.shortcut = readShortcut(shortcutValue.toObject());
        }

        if (!parseCommandCapability(command.value(QStringLiteral("capability")).toString(),
                                    &descriptor.capability))
        {
            m_loadError = QStringLiteral("command-catalog/unknown-capability");
            return;
        }

        if (!parseCommandAvailability(command.value(QStringLiteral("availability")).toString(),
                                      &descriptor.availability))
        {
            m_loadError = QStringLiteral("command-catalog/unknown-availability");
            return;
        }

        descriptor.cancellable = command.value(QStringLiteral("cancellable")).toBool();

        const QJsonValue parametersValue = command.value(QStringLiteral("parameters"));
        if (!parametersValue.isArray())
        {
            m_loadError = QStringLiteral("command-catalog/missing-parameters");
            return;
        }

        for (const QJsonValue& parameterValue : parametersValue.toArray())
        {
            if (!parameterValue.isObject())
            {
                m_loadError = QStringLiteral("command-catalog/malformed-parameter");
                return;
            }

            const QJsonObject parameterObject = parameterValue.toObject();
            CommandParameterSpec parameter;
            parameter.name = parameterObject.value(QStringLiteral("name")).toString();
            parameter.required = parameterObject.value(QStringLiteral("required")).toBool();

            if (parameter.name.isEmpty() ||
                !parseCommandParameterType(parameterObject.value(QStringLiteral("type")).toString(),
                                           &parameter.type))
            {
                m_loadError = QStringLiteral("command-catalog/malformed-parameter");
                return;
            }

            descriptor.parameters.append(parameter);
        }

        indexById.insert(descriptor.id, descriptors.size());
        descriptors.append(std::move(descriptor));
    }

    if (descriptors.isEmpty())
    {
        m_loadError = QStringLiteral("command-catalog/empty-contract");
        return;
    }

    m_descriptors = std::move(descriptors);
    m_indexById = std::move(indexById);
    m_loaded = true;
    m_loadError.clear();
}

const CommandDescriptor* CommandCatalog::descriptor(const CommandId& id) const
{
    const auto it = m_indexById.constFind(id);
    if (it == m_indexById.constEnd())
    {
        return nullptr;
    }

    return &m_descriptors.at(*it);
}

bool CommandCatalog::setHandler(const CommandId& id, Handler handler)
{
    const CommandDescriptor* known = descriptor(id);
    if (!known || !known->isImplemented() || !handler.invoke)
    {
        return false;
    }

    m_handlers.insert(id, std::move(handler));
    return true;
}

void CommandCatalog::clearHandler(const CommandId& id)
{
    m_handlers.remove(id);
}

bool CommandCatalog::isEnabled(const CommandId& id) const
{
    return m_enabled.value(id, false);
}

void CommandCatalog::setEnabled(const CommandId& id, bool enabled)
{
    setEnabledBatch({ { id, enabled } });
}

void CommandCatalog::setEnabledBatch(const QHash<CommandId, bool>& availability)
{
    bool changed = false;
    for (auto it = availability.cbegin(); it != availability.cend(); ++it)
    {
        if (!descriptor(it.key()) || m_enabled.value(it.key(), false) == it.value())
        {
            continue;
        }

        m_enabled.insert(it.key(), it.value());
        changed = true;
    }

    if (changed)
    {
        Q_EMIT availabilityChanged();
    }
}

void CommandCatalog::invalidateAvailability()
{
    if (m_enabled.isEmpty())
    {
        return;
    }

    m_enabled.clear();
    Q_EMIT availabilityChanged();
}

CommandInvocationId CommandCatalog::reject(const CommandId& id,
                                           CommandTerminalState state,
                                           QString typedError)
{
    CommandResult result;
    result.invocation = InvalidCommandInvocation;
    result.command = id;
    result.state = state;
    result.typedError = std::move(typedError);
    Q_EMIT invocationFinished(result);
    return InvalidCommandInvocation;
}

bool CommandCatalog::validateParameters(const CommandDescriptor& descriptor,
                                        const QVariantMap& parameters,
                                        QString* typedError) const
{
    for (const CommandParameterSpec& parameter : descriptor.parameters)
    {
        const auto it = parameters.constFind(parameter.name);
        if (it == parameters.constEnd())
        {
            if (parameter.required)
            {
                *typedError = QStringLiteral("command/missing-parameter");
                return false;
            }
            continue;
        }

        bool typeMatches = false;
        switch (parameter.type)
        {
            case CommandParameterType::String:
                typeMatches = it->typeId() == QMetaType::QString;
                break;

            case CommandParameterType::Integer:
                typeMatches = it->typeId() == QMetaType::Int || it->typeId() == QMetaType::LongLong ||
                              it->typeId() == QMetaType::UInt || it->typeId() == QMetaType::ULongLong;
                break;

            case CommandParameterType::Number:
                typeMatches = it->typeId() == QMetaType::Double || it->typeId() == QMetaType::Float ||
                              it->typeId() == QMetaType::Int || it->typeId() == QMetaType::LongLong;
                break;

            case CommandParameterType::Boolean:
                typeMatches = it->typeId() == QMetaType::Bool;
                break;
        }

        if (!typeMatches)
        {
            *typedError = QStringLiteral("command/invalid-parameter-type");
            return false;
        }
    }

    // Unknown parameters are refused rather than dropped. A host that misspells
    // a parameter name would otherwise get a command that ran with a default it
    // never asked for.
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it)
    {
        const bool declared = std::any_of(descriptor.parameters.constBegin(),
                                          descriptor.parameters.constEnd(),
                                          [&it](const CommandParameterSpec& parameter)
                                          { return parameter.name == it.key(); });
        if (!declared)
        {
            *typedError = QStringLiteral("command/unknown-parameter");
            return false;
        }
    }

    return true;
}

CommandInvocationId CommandCatalog::invoke(const CommandId& id, const QVariantMap& parameters)
{
    if (!m_loaded)
    {
        return reject(id, CommandTerminalState::Unavailable, QStringLiteral("command-catalog/not-loaded"));
    }

    const CommandDescriptor* known = descriptor(id);
    if (!known)
    {
        // The contract is the whole ID space. An unknown ID is a routing bug in
        // the caller, not a command that happens to be missing today.
        return reject(id, CommandTerminalState::Unavailable, QStringLiteral("command/unknown"));
    }

    if (!known->isImplemented())
    {
        return reject(id, CommandTerminalState::NotImplemented, QStringLiteral("command/not-implemented"));
    }

    const auto handlerIt = m_handlers.constFind(id);
    if (handlerIt == m_handlers.constEnd())
    {
        return reject(id, CommandTerminalState::Unavailable, QStringLiteral("command/no-handler"));
    }

    if (!isEnabled(id))
    {
        return reject(id, CommandTerminalState::Unavailable, QStringLiteral("command/unavailable"));
    }

    QString typedError;
    if (!validateParameters(*known, parameters, &typedError))
    {
        return reject(id, CommandTerminalState::Failed, std::move(typedError));
    }

    const CommandInvocationId invocation = m_nextInvocation++;

    PendingInvocation pending;
    pending.command = id;
    pending.cancellable = known->cancellable;
    m_pending.insert(invocation, pending);

    // Copied on purpose: a handler is allowed to finish synchronously, which
    // may re-enter and rehash m_handlers.
    const Handler handler = *handlerIt;
    handler.invoke(invocation, parameters);
    return invocation;
}

bool CommandCatalog::cancelInvocation(CommandInvocationId invocation)
{
    const auto it = m_pending.find(invocation);
    if (it == m_pending.end() || !it->cancellable || it->cancellationRequested)
    {
        return false;
    }

    const auto handlerIt = m_handlers.constFind(it->command);
    if (handlerIt == m_handlers.constEnd() || !handlerIt->cancel)
    {
        return false;
    }

    it->cancellationRequested = true;

    const std::function<void(CommandInvocationId)> cancel = handlerIt->cancel;
    cancel(invocation);
    return true;
}

bool CommandCatalog::finishInvocation(CommandInvocationId invocation,
                                      CommandTerminalState state,
                                      QString typedError)
{
    const auto it = m_pending.constFind(invocation);
    if (it == m_pending.constEnd())
    {
        return false;
    }

    CommandResult result;
    result.invocation = invocation;
    result.command = it->command;
    result.state = state;
    result.typedError = std::move(typedError);

    m_pending.erase(it);
    Q_EMIT invocationFinished(result);
    return true;
}

bool CommandCatalog::isPending(CommandInvocationId invocation) const
{
    return m_pending.contains(invocation);
}

int CommandCatalog::pendingInvocationCount() const
{
    return int(m_pending.size());
}

}   // namespace pdfinteraction
