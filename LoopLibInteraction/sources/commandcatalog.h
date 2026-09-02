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

#ifndef COMMANDCATALOG_H
#define COMMANDCATALOG_H

#include "commanddescriptor.h"

#include <QHash>
#include <QObject>
#include <QVariantMap>

#include <functional>

namespace pdfinteraction
{

/// The one command registry.
///
/// Descriptors are loaded from docs/loop-shell-actions.json, which is also the
/// Editor action policy verified against the current Quick shell contract.
/// There is exactly one ID set: a presentation host binds menus, shortcuts, and
/// controls to descriptors and invokes only through this catalog, so it cannot
/// grow a private action tree of its own.
///
/// The catalog owns descriptors, availability, and invocation identity. It does
/// not own document lifecycle, mutation, or a scheduler; implemented commands
/// are handlers registered by whoever does own those (pdfinteraction::DocumentFacade
/// for the document lifecycle). There is deliberately no generic "invoke any
/// QObject method" route.
class CommandCatalog final : public QObject
{
    Q_OBJECT

public:
    /// Loads the catalog from the contract embedded at build time.
    explicit CommandCatalog(QObject* parent = nullptr);

    /// Loads the catalog from an explicit contract payload. Used by tests and by
    /// any host that must fail loudly on a corrupt resource rather than start
    /// with a silently empty command set.
    static CommandCatalog* fromContract(const QByteArray& contract, QObject* parent = nullptr);

    ~CommandCatalog() override;

    CommandCatalog(const CommandCatalog&) = delete;
    CommandCatalog& operator=(const CommandCatalog&) = delete;

    /// Returns whether the contract parsed. A catalog that failed to load
    /// answers every invocation with Unavailable instead of pretending the
    /// command does not exist.
    bool isLoaded() const noexcept { return m_loaded; }
    QString loadError() const { return m_loadError; }

    const QList<CommandDescriptor>& descriptors() const noexcept { return m_descriptors; }

    /// Returns the descriptor, or nullptr when the ID is not in the contract.
    const CommandDescriptor* descriptor(const CommandId& id) const;

    struct Handler
    {
        /// Runs the command. The catalog has already validated the ID, the
        /// availability, and the parameters.
        std::function<void(CommandInvocationId, const QVariantMap&)> invoke;

        /// Requests cancellation. Optional; a command whose descriptor is not
        /// cancellable never receives this.
        std::function<void(CommandInvocationId)> cancel;
    };

    /// Registers the implementation of a command. Registering a handler for an
    /// unknown ID, or for a command the contract marks Declared, is refused —
    /// the contract decides what exists, not the caller.
    bool setHandler(const CommandId& id, Handler handler);

    /// Removes a handler. A handler owner must call this before it is destroyed;
    /// the catalog does not outlive-check the callables it was given.
    void clearHandler(const CommandId& id);

    /// Availability snapshot. Everything is disabled until a handler owner says
    /// otherwise, so a command can never appear usable before the state that
    /// makes it usable exists.
    bool isEnabled(const CommandId& id) const;
    void setEnabled(const CommandId& id, bool enabled);

    /// Applies one logical availability update atomically. Unknown IDs are
    /// ignored and observers see at most one availabilityChanged signal.
    void setEnabledBatch(const QHash<CommandId, bool>& availability);

    /// Drops the whole availability snapshot. Document replacement invalidates
    /// it: availability computed against the previous document is not evidence
    /// about the new one.
    void invalidateAvailability();

    /// Invokes a command. Returns the invocation ID, or InvalidCommandInvocation
    /// when nothing was started — in which case a terminal CommandResult has
    /// already been emitted saying why. An unknown ID is reported, never
    /// silently ignored.
    CommandInvocationId invoke(const CommandId& id, const QVariantMap& parameters = {});

    /// Requests cancellation of a live invocation. Returns false when the
    /// invocation is unknown, already terminal, or not cancellable.
    bool cancelInvocation(CommandInvocationId invocation);

    /// Reports a terminal state for a live invocation. Called by handler owners;
    /// a second call for the same invocation is ignored, so a terminal state is
    /// terminal.
    bool finishInvocation(CommandInvocationId invocation,
                          CommandTerminalState state,
                          QString typedError = QString());

    /// Returns whether an invocation is still live.
    bool isPending(CommandInvocationId invocation) const;

    int pendingInvocationCount() const;

signals:
    /// Emitted when the availability snapshot changes, including when it is
    /// invalidated wholesale.
    void availabilityChanged();

    /// Emitted exactly once per invocation, including for invocations that never
    /// started.
    void invocationFinished(pdfinteraction::CommandResult result);

private:
    CommandCatalog(const QByteArray& contract, QObject* parent);

    void load(const QByteArray& contract);
    CommandInvocationId reject(const CommandId& id, CommandTerminalState state, QString typedError);
    bool validateParameters(const CommandDescriptor& descriptor,
                            const QVariantMap& parameters,
                            QString* typedError) const;

    struct PendingInvocation
    {
        CommandId command;
        bool cancellable = false;
        bool cancellationRequested = false;
    };

    bool m_loaded = false;
    QString m_loadError;
    QList<CommandDescriptor> m_descriptors;
    QHash<CommandId, qsizetype> m_indexById;
    QHash<CommandId, Handler> m_handlers;
    QHash<CommandId, bool> m_enabled;
    QHash<CommandInvocationId, PendingInvocation> m_pending;
    CommandInvocationId m_nextInvocation = 1;
};

}   // namespace pdfinteraction

#endif   // COMMANDCATALOG_H
