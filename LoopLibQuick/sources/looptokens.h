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


#ifndef LOOPTOKENS_H
#define LOOPTOKENS_H

#include "loopquickglobal.h"

#include <QColor>

namespace pdfquick::tokens
{

// Spacing — 4px base grid. Mirrors docs/quick-design-tokens.json `spacing.values_px`.
inline constexpr int SpaceXs = 4;
inline constexpr int SpaceS = 8;
inline constexpr int SpaceM = 12;
inline constexpr int SpaceL = 16;
inline constexpr int SpaceXl = 24;
inline constexpr int SpaceXxl = 32;

// Typography — mirrors docs/quick-design-tokens.json `typography`.
inline constexpr int TypeBodyPx = 14;
inline constexpr int TypeSmallPx = 12;
inline constexpr int TypeHeadingPx = 24;

// Focus geometry — mirrors docs/quick-design-tokens.json `focus`.
inline constexpr int FocusOutlineWidthPx = 2;
inline constexpr int FocusOutlineOffsetPx = 2;

// Density — mirrors docs/quick-design-tokens.json `density`.
inline constexpr int MinimumPointerTargetPx = 44;
inline constexpr int MinimumKeyboardTargetPx = 32;

/// The theme a `ColorRole` resolves against. `HighContrast` is a distinct theme
/// rather than a flag on `Dark`/`Light`: every role has a value in all three.
enum class LoopTheme
{
    Dark,
    Light,
    HighContrast
};

/// Semantic colour role. Named for what a surface or piece of text *is*, never
/// for a colour. A call site that reaches for a raw QColor or a hex literal
/// instead of a role is a design-system violation, not a shortcut.
enum class ColorRole
{
    SurfaceBase,
    SurfacePanel,
    SurfaceOverlay,

    TextPrimary,
    TextSecondary,
    TextDisabled,

    SeverityError,
    SeverityWarning,
    SeverityInfo,

    /// The "no findings" treatment. Distinct from `StateIncomplete` and
    /// `StateNotChecked` by more than hue — see resolveStateVisual().
    Success,

    /// A check that did not run to completion (budget exceeded, skipped,
    /// unsupported). NOT a severity: never resolves to the `Success` role.
    StateIncomplete,

    /// No run exists yet for this revision. Never the `Success` role.
    StateNotChecked,

    FocusRing,
    DestructiveAction
};

/// Resolves one semantic role to a concrete colour for `theme`. The only place
/// in the Loop UI that is allowed to know a hex value; every other surface goes
/// through this function (or through a component built on it, such as
/// resolveStateVisual()).
LOOPLIBQUICK_EXPORT QColor color(ColorRole role, LoopTheme theme);

}   // namespace pdfquick::tokens

#endif   // LOOPTOKENS_H
