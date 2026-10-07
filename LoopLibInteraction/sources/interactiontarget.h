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

#ifndef INTERACTIONTARGET_H
#define INTERACTIONTARGET_H

#include "interactionglobal.h"

#include <QMetaType>
#include <QRectF>
#include <QString>

namespace pdfinteraction
{

/// What kind of thing an interaction is aimed at.
///
/// The order is the hit-test precedence, highest first, and it is the reverse of
/// the order the overlay z-bands paint in. One enum for both means a marker
/// cannot be drawn on top of something that wins the hit test under it.
///
/// Being selected is deliberately not a kind. A target's identity is its kind
/// plus its id, and promoting a hit to a "selected" kind would make the same
/// object compare unequal to itself depending on what was selected at the time.
/// HitTestDispatcher ranks the selected target above its peers without touching
/// what it is.
enum class InteractionTargetKind
{
    /// A manipulator on the active selection. Always wins: a handle that loses
    /// to the object it sits on cannot be grabbed.
    DragHandle,

    /// A preflight finding or an evidence record, addressed by its stable id.
    Finding,

    /// A guide, ruler or measurement.
    Guide,

    /// A page box outline: media, crop, trim, bleed, art.
    PageBox,

    /// The page itself, hit anywhere no other target claims.
    Page,

    None
};

const char* getInteractionTargetKindName(InteractionTargetKind kind);

/// What a completed drag of a target kind is allowed to become (issue #206).
///
/// Every kind has exactly one answer, so a completed drag is never silently
/// dropped: it either runs through the command catalog or the owner reports a
/// typed refusal. A kind moves to Commit only with its own contract (#141),
/// never by being added here first.
enum class DragCommitDisposition
{
    /// The owner routes the session through `CommandCatalog` (ADR-012).
    Commit,

    /// The owner changes nothing and tells the operator why.
    Refused
};

/// Only a page box is document truth a move may edit. A finding is derived
/// evidence, a guide is presentation state, and there is no text target kind.
DragCommitDisposition getDragCommitDisposition(InteractionTargetKind kind);

/// One addressable thing, identified the way its owner already identifies it.
///
/// `id` is a stable id from the producing domain -- pdf::PDFEvidenceRecord::id
/// for a finding, a page box name for a box -- never a pointer, an index into a
/// container, or anything that changes when a list is refiltered. P4-S8's
/// findings model and P4-S9's inspector resolve the same value, which is what
/// makes "one selection identity" true rather than aspirational.
///
/// It deliberately carries no pdf::PDFObjectReference and no document pointer.
/// A target is a name for something, not a handle to it; resolving it back to
/// document truth is Core's job, through the command catalog.
struct InteractionTarget
{
    InteractionTargetKind kind = InteractionTargetKind::None;
    int pageIndex = -1;
    QString id;

    /// Bounds in page space, for the overlay and for tie-breaking. May be null
    /// when the producing domain has no geometry for this target.
    QRectF pageBounds;

    bool isValid() const { return kind != InteractionTargetKind::None; }
    bool operator==(const InteractionTarget& other) const = default;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::InteractionTarget)
Q_DECLARE_METATYPE(pdfinteraction::InteractionTargetKind)

#endif   // INTERACTIONTARGET_H
