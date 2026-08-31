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

#ifndef INPUTINTENT_H
#define INPUTINTENT_H

#include "interactionglobal.h"

#include <QPoint>
#include <QPointF>
#include <Qt>

namespace pdfinteraction
{

/// What a host reports happened, not which class reported it.
///
/// These are the only input types that cross into the interaction layer. A
/// QMouseEvent, a QWheelEvent, a QKeyEvent and a QQuickItem's event handlers all
/// reduce to the same three values, so the Quick canvas and its tests drive
/// identical code. That is what makes a recorded session replayable:
/// a QEvent cannot be constructed from a trace file with its timestamp intact,
/// and a value can.
enum class PointerAction
{
    Press,
    Move,
    Release,

    /// The host revoked the gesture: capture lost, the window deactivated, a
    /// touch was rejected. Distinct from Release, which completes.
    Cancel,

    /// The pointer left the surface. Clears hover; never completes a drag.
    Leave
};

enum class KeyAction
{
    Press,
    Release
};

/// A monotonic nanosecond stamp taken by the host, plus the ordinal of this
/// intent within its session.
///
/// The clock is the host's, injected rather than read here, because a recorder
/// that calls QDateTime::currentMSecsSinceEpoch() cannot be replayed and a test
/// that waits on a real clock is a flake. `sequence` is what a trace replays in
/// order, and it is also how a late-arriving intent is recognized.
struct InputStamp
{
    qint64 monotonicNs = 0;
    quint64 sequence = 0;

    bool operator==(const InputStamp& other) const = default;
};

/// Pointer input in viewport pixels.
///
/// `buttons` is the state after the action, matching QMouseEvent::buttons(), so
/// a Press carries the pressed button and a Release does not. `button` is the
/// one that changed, and is NoButton for Move.
struct PointerIntent
{
    InputStamp stamp;
    PointerAction action = PointerAction::Move;
    QPoint positionPx;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;

    bool operator==(const PointerIntent& other) const = default;
};

/// Wheel or trackpad input.
///
/// Both angle and pixel deltas are carried because they are not
/// interchangeable: a mouse wheel reports 120ths of a degree in discrete
/// notches, a trackpad reports pixels continuously, and collapsing them here
/// would bake one host's convention into the neutral layer.
struct WheelIntent
{
    InputStamp stamp;
    QPoint positionPx;
    QPoint angleDelta;
    QPoint pixelDelta;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;

    bool operator==(const WheelIntent& other) const = default;
};

/// Key input. `key` is a Qt::Key value; no text is carried, because key text can
/// be document content being typed and this type is recorded into traces.
struct KeyIntent
{
    InputStamp stamp;
    KeyAction action = KeyAction::Press;
    int key = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    bool autoRepeat = false;

    bool operator==(const KeyIntent& other) const = default;
};

/// Focus and capture changes a host must report, because a drag that survives
/// them commits a transform the user has stopped steering.
enum class HostNotification
{
    FocusLost,
    CaptureLost,
    WindowDeactivated
};

const char* getPointerActionName(PointerAction action);
const char* getHostNotificationName(HostNotification notification);

}   // namespace pdfinteraction

#endif   // INPUTINTENT_H
