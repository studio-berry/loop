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


#include "loopstatevisual.h"

#include "preflightengine.h"

namespace pdfquick::tokens
{

namespace
{

LoopStateVisual fromFinding(const pdf::PreflightFinding& finding)
{
    const QString severity = finding.severity.trimmed();

    if (severity.compare(QLatin1String("error"), Qt::CaseInsensitive) == 0)
    {
        return { StateKind::Error, ColorRole::SeverityError, StateIcon::FilledCircle };
    }
    if (severity.compare(QLatin1String("warning"), Qt::CaseInsensitive) == 0)
    {
        return { StateKind::Warning, ColorRole::SeverityWarning, StateIcon::FilledTriangle };
    }
    if (severity.compare(QLatin1String("info"), Qt::CaseInsensitive) == 0)
    {
        return { StateKind::Info, ColorRole::SeverityInfo, StateIcon::FilledSquare };
    }

    // profile.schema.json admits only error/warning/info. A finding with
    // anything else is data this build does not understand -- the safe
    // reading is "cannot vouch for this", not "no problem here", so it takes
    // the same never-green treatment as an incomplete check rather than
    // silently falling through to Passed.
    return { StateKind::Incomplete, ColorRole::StateIncomplete, StateIcon::Hatched };
}

LoopStateVisual fromStatus(const pdf::PreflightCheckStatus& status)
{
    if (status.status.compare(QLatin1String("ok"), Qt::CaseInsensitive) == 0)
    {
        return { StateKind::Passed, ColorRole::Success, StateIcon::Checkmark };
    }

    // Every other status literal this build emits -- failed, warning, skipped,
    // incomplete, unsupported -- and any literal a future check adds all take
    // this branch. That is deliberately coarser than the run-level verdict in
    // pdf::reducePreflightVerdict(): a caller presenting one check's
    // completion, without a specific finding to show, only ever needs to know
    // "clean pass" from "not that", and the second must never render as the
    // first.
    return { StateKind::Incomplete, ColorRole::StateIncomplete, StateIcon::Hatched };
}

}   // namespace

LoopStateVisual resolveStateVisual(const pdf::PreflightFinding* finding,
                                   const pdf::PreflightCheckStatus* status,
                                   const pdf::PreflightDecision* decision,
                                   const QString& currentDocumentDigest,
                                   const QString& currentProfileDigest)
{
    // Checked first and unconditionally: a waived finding is presented as
    // waived regardless of its severity or the check's completion status.
    // resolveState() -- not the stored kind alone -- decides "active", so a
    // decision recorded against a document revision or profile that no longer
    // matches falls through instead of masking the finding (mirrors
    // PreflightDecision::countsForSignoff(), issue #126).
    if (decision != nullptr && decision->kind == pdf::PreflightDecisionKind::Waive)
    {
        const pdf::PreflightDecisionState state = decision->resolveState(currentDocumentDigest, currentProfileDigest);
        if (state == pdf::PreflightDecisionState::Active)
        {
            return { StateKind::Waived, ColorRole::SeverityWarning, StateIcon::BadgeOverlay };
        }
    }

    if (finding != nullptr)
    {
        return fromFinding(*finding);
    }

    if (status != nullptr)
    {
        return fromStatus(*status);
    }

    // No finding, no check status, no active waiver: nothing has run for this
    // revision yet.
    return { StateKind::NotChecked, ColorRole::StateNotChecked, StateIcon::Outline };
}

}   // namespace pdfquick::tokens
