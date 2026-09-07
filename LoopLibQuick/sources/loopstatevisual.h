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

/// Non-colour shape. BadgeOverlay is drawn on top of the finding's severity treatment.
enum class StateIcon
{
    FilledCircle,
    FilledTriangle,
    FilledSquare,
    Hatched,
    Outline,
    Checkmark,
    BadgeOverlay
};

struct LoopStateVisual
{
    StateKind kind = StateKind::NotChecked;
    ColorRole colorRole = ColorRole::StateNotChecked;
    StateIcon icon = StateIcon::Outline;
    QString accessibleName;
};

LOOPLIBQUICK_EXPORT QString stateAccessibleName(StateKind kind);

/// Canonical finding/check presentation. Incomplete and active Waive never resolve as Passed.
LOOPLIBQUICK_EXPORT LoopStateVisual resolveStateVisual(const pdf::PreflightFinding* finding,
                                                       const pdf::PreflightCheckStatus* status,
                                                       const pdf::PreflightDecision* decision,
                                                       const QString& currentDocumentDigest = QString(),
                                                       const QString& currentProfileDigest = QString());

}   // namespace pdfquick::tokens

#endif   // LOOPSTATEVISUAL_H
