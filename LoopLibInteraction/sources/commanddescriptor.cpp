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

#include "commanddescriptor.h"

namespace pdfinteraction
{

namespace
{

// The spelling on both sides of these tables is the contract in
// docs/schemas/loop-shell-actions.schema.json. Changing one without the other
// is what scripts/verify-command-catalog.py and UnitTestsDocumentFacade exist to
// catch.
struct CapabilityName
{
    CommandCapability capability;
    const char* name;
};

constexpr CapabilityName CapabilityNames[] = {
    { CommandCapability::Unclassified, "unclassified" },
    { CommandCapability::None, "none" },
    { CommandCapability::DocumentRead, "document.read" },
    { CommandCapability::DocumentWrite, "document.write" },
    { CommandCapability::DocumentModify, "document.modify" },
    { CommandCapability::Application, "application" }
};

struct AvailabilityName
{
    CommandAvailability availability;
    const char* name;
};

constexpr AvailabilityName AvailabilityNames[] = {
    { CommandAvailability::Implemented, "implemented" },
    { CommandAvailability::Declared, "declared" }
};

struct ParameterTypeName
{
    CommandParameterType type;
    const char* name;
};

constexpr ParameterTypeName ParameterTypeNames[] = {
    { CommandParameterType::String, "string" },
    { CommandParameterType::Integer, "integer" },
    { CommandParameterType::Number, "number" },
    { CommandParameterType::Boolean, "boolean" }
};

}   // namespace

const char* getCommandCapabilityName(CommandCapability capability)
{
    for (const CapabilityName& entry : CapabilityNames)
    {
        if (entry.capability == capability)
        {
            return entry.name;
        }
    }

    return "unclassified";
}

const char* getCommandAvailabilityName(CommandAvailability availability)
{
    for (const AvailabilityName& entry : AvailabilityNames)
    {
        if (entry.availability == availability)
        {
            return entry.name;
        }
    }

    return "declared";
}

const char* getCommandParameterTypeName(CommandParameterType type)
{
    for (const ParameterTypeName& entry : ParameterTypeNames)
    {
        if (entry.type == type)
        {
            return entry.name;
        }
    }

    return "string";
}

const char* getCommandTerminalStateName(CommandTerminalState state)
{
    switch (state)
    {
        case CommandTerminalState::Completed:
            return "completed";

        case CommandTerminalState::Cancelled:
            return "cancelled";

        case CommandTerminalState::Failed:
            return "failed";

        case CommandTerminalState::NotImplemented:
            return "not-implemented";

        case CommandTerminalState::Unavailable:
            return "unavailable";
    }

    return "failed";
}

bool parseCommandCapability(const QString& name, CommandCapability* capability)
{
    for (const CapabilityName& entry : CapabilityNames)
    {
        if (name == QLatin1String(entry.name))
        {
            if (capability)
            {
                *capability = entry.capability;
            }
            return true;
        }
    }

    return false;
}

bool parseCommandAvailability(const QString& name, CommandAvailability* availability)
{
    for (const AvailabilityName& entry : AvailabilityNames)
    {
        if (name == QLatin1String(entry.name))
        {
            if (availability)
            {
                *availability = entry.availability;
            }
            return true;
        }
    }

    return false;
}

bool parseCommandParameterType(const QString& name, CommandParameterType* type)
{
    for (const ParameterTypeName& entry : ParameterTypeNames)
    {
        if (name == QLatin1String(entry.name))
        {
            if (type)
            {
                *type = entry.type;
            }
            return true;
        }
    }

    return false;
}

}   // namespace pdfinteraction
