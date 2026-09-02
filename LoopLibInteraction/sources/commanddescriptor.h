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

#ifndef COMMANDDESCRIPTOR_H
#define COMMANDDESCRIPTOR_H

#include "interactionglobal.h"

#include <QList>
#include <QMetaType>
#include <QString>

namespace pdfinteraction
{

/// Stable command identifier. These are the Editor action IDs recorded in
/// docs/loop-shell-actions.json; a command ID that is not in that file does not
/// exist. Reusing the existing IDs verbatim is what makes a second action
/// registry impossible rather than merely discouraged.
using CommandId = QString;

/// Identifies one in-flight invocation. Zero is never a live invocation.
using CommandInvocationId = quint64;

inline constexpr CommandInvocationId InvalidCommandInvocation = 0;

/// What a command is permitted to touch. `Unclassified` is legal only while the
/// command is Declared; scripts/verify-command-catalog.py rejects an Implemented
/// command that never got classified.
enum class CommandCapability
{
    Unclassified,
    None,
    DocumentRead,
    DocumentWrite,
    DocumentModify,
    Application
};

/// Whether a handler exists. A Declared command has a descriptor — so menus,
/// shortcuts, and routing are complete — but invoking it yields NotImplemented
/// and mutates nothing. This keeps the catalog honest instead of pretending the
/// Quick product already reimplemented every Widgets action.
enum class CommandAvailability
{
    Implemented,
    Declared
};

enum class CommandParameterType
{
    String,
    Integer,
    Number,
    Boolean
};

/// Terminal states an invocation can reach. Cancelled and Failed are distinct
/// and neither is success — the same rule architecture invariant I05 pins for
/// pdf::PDFJobScheduler.
enum class CommandTerminalState
{
    Completed,
    Cancelled,
    Failed,
    NotImplemented,
    Unavailable
};

struct CommandParameterSpec
{
    QString name;
    CommandParameterType type = CommandParameterType::String;
    bool required = false;

    bool operator==(const CommandParameterSpec&) const = default;
};

/// The declared form of a keyboard shortcut, not a resolved QKeySequence.
///
/// Resolution is a presentation concern: the shell that builds menus decides how
/// a QKeySequence::StandardKey name becomes a key sequence on the running
/// platform. The neutral layer owns which shortcut a command has, which is what
/// keeps QML from inventing shortcut literals of its own. The platform choice
/// between the declared default and a Windows override is already made when a
/// descriptor is loaded.
struct CommandShortcut
{
    /// A QKeySequence::StandardKey enumerator name, or empty.
    QString standardKey;

    /// A literal sequence such as "Ctrl+Shift+B", or empty.
    QString sequence;

    bool isEmpty() const { return standardKey.isEmpty() && sequence.isEmpty(); }
    bool operator==(const CommandShortcut&) const = default;
};

struct CommandDescriptor
{
    CommandId id;
    QString labelKey;
    CommandShortcut shortcut;
    QList<CommandParameterSpec> parameters;
    CommandCapability capability = CommandCapability::Unclassified;
    bool cancellable = false;
    CommandAvailability availability = CommandAvailability::Declared;

    /// Routing metadata carried through from the shell contract, so a Quick
    /// workspace rail can place a command without a second lookup table.
    QString disposition;
    QString target;

    bool isImplemented() const { return availability == CommandAvailability::Implemented; }
    bool operator==(const CommandDescriptor&) const = default;
};

struct CommandResult
{
    CommandInvocationId invocation = InvalidCommandInvocation;
    CommandId command;
    CommandTerminalState state = CommandTerminalState::Failed;

    /// Policy-safe error code, never a file path or document content. Empty on
    /// success. See docs/QUICK_SHELL_THREAT_MODEL.md.
    QString typedError;

    bool isSuccess() const { return state == CommandTerminalState::Completed; }
    bool operator==(const CommandResult&) const = default;
};

const char* getCommandCapabilityName(CommandCapability capability);
const char* getCommandAvailabilityName(CommandAvailability availability);
const char* getCommandParameterTypeName(CommandParameterType type);
const char* getCommandTerminalStateName(CommandTerminalState state);

/// Parse helpers used by the catalog loader. They return false rather than
/// guessing, so an unknown enumerator in the contract is a load error and not a
/// silently defaulted descriptor.
bool parseCommandCapability(const QString& name, CommandCapability* capability);
bool parseCommandAvailability(const QString& name, CommandAvailability* availability);
bool parseCommandParameterType(const QString& name, CommandParameterType* type);

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::CommandResult)

#endif   // COMMANDDESCRIPTOR_H
