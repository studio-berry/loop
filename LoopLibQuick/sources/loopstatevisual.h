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
/// `ColorRole` (below) even though today it maps one-to-one, because a state
/// is a fact about a finding and a colour role is a fact about a pixel; a
/// future high-contrast or print treatment that wants to give two states the
/// same colour role must still tell them apart by `kind`.
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
/// treatment, not instead of it -- a waived error still shows as an error with
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
};

/// Single source of truth for finding/check presentation (issue #194). Every
/// surface that draws a finding, a check row, or a run summary -- finding
/// cards, the report dock, canvas overlays, the Inspector (#127), the status
/// bar -- calls this; none derives its own colour or icon from `severity`,
/// `status`, or a decision's kind directly.
///
/// `finding` is the specific finding being presented, or null when the caller
/// is presenting a check's overall status rather than one of its findings (for
/// example, a check row with zero findings). `status` is the
/// PreflightCheckStatus for the check `finding` belongs to (or the check being
/// summarised), or null when no run exists yet for the current document
/// revision. `decision` is the operator decision recorded against
/// `finding->stableId()`, or null when none was recorded; `currentDocumentDigest`
/// and `currentProfileDigest` are passed through to
/// `PreflightDecision::resolveState()` so a decision made against a stale
/// document or profile is never read as active (mirrors
/// `PreflightDecision::countsForSignoff()`, issue #126).
///
/// Two invariants hold for every input combination and are asserted by
/// tst_loopstatevisualtest.cpp:
///
///   - `StateKind::Incomplete` never resolves to the same colour role or icon
///     as `StateKind::Passed`. An incomplete check must never render as a
///     clean pass (issue #133).
///   - An active Waive decision never resolves to `StateKind::Passed`. Waived
///     always renders as `StateKind::Waived`, distinct from Passed.
LOOPLIBQUICK_EXPORT LoopStateVisual resolveStateVisual(const pdf::PreflightFinding* finding,
                                                       const pdf::PreflightCheckStatus* status,
                                                       const pdf::PreflightDecision* decision,
                                                       const QString& currentDocumentDigest = QString(),
                                                       const QString& currentProfileDigest = QString());

}   // namespace pdfquick::tokens

#endif   // LOOPSTATEVISUAL_H
