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


#ifndef LOOPSTATEVISUAL_H
#define LOOPSTATEVISUAL_H

#include "loopquickglobal.h"
#include "looptokens.h"

#include <QString>

namespace pdf
{
struct PreflightFinding;
struct PreflightCheckStatus;
struct PreflightDecision;
}   // namespace pdf

namespace pdfquick::tokens
{

/// The finding/check state a surface is presenting. Kept separate from
/// `ColorRole` even though today it maps one-to-one, because a state is a fact
/// about a finding and a colour role is a fact about a pixel.
enum class StateKind
{
    Error,
    Warning,
    Info,
    Incomplete,
    NotChecked,
    Passed,
    Waived
};

/// Shape carries the state distinction alongside colour, so the mapping
/// survives colour-blindness and greyscale printing (docs/ACCESSIBILITY_BASELINE.md,
/// issue #25). `BadgeOverlay` is drawn in addition to the underlying severity
/// treatment, not instead of it — a waived error still shows as an error with
/// a badge, it never becomes indistinguishable from a plain warning.
enum class StateIcon
{
    FilledCircle,   // Error
    FilledTriangle,   // Warning
    FilledSquare,   // Info
    Hatched,   // Incomplete
    Outline,   // Not checked
    Checkmark,   // Passed
    BadgeOverlay   // Waived
};

struct LoopStateVisual
{
    StateKind kind = StateKind::NotChecked;
    ColorRole colorRole = ColorRole::StateNotChecked;
    StateIcon icon = StateIcon::Outline;
    /// Non-colour text cue. Surfaces must expose this (or a translation of it)
    /// as the accessible name; colour is never the only state channel.
    QString accessibleName;
};

/// Stable English accessible name for `kind`. Used by resolveStateVisual() and
/// by any surface that needs the label without a full visual mapping.
LOOPLIBQUICK_EXPORT QString stateAccessibleName(StateKind kind);

/// Single source of truth for finding/check presentation (issue #194). Every
/// surface that draws a finding, a check row, or a run summary calls this;
/// none derives its own colour, icon, or accessible name from `severity`,
/// `status`, or a decision's kind directly.
///
/// Two invariants hold for every input combination and are asserted by
/// tst_loopstatevisualtest.cpp:
///
///   - `StateKind::Incomplete` never resolves to the same colour role, icon,
///     or accessible name as `StateKind::Passed`.
///   - An active Waive decision never resolves to `StateKind::Passed`.
LOOPLIBQUICK_EXPORT LoopStateVisual resolveStateVisual(const pdf::PreflightFinding* finding,
                                                       const pdf::PreflightCheckStatus* status,
                                                       const pdf::PreflightDecision* decision,
                                                       const QString& currentDocumentDigest = QString(),
                                                       const QString& currentProfileDigest = QString());

}   // namespace pdfquick::tokens

#endif   // LOOPSTATEVISUAL_H
