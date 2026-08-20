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

#include "pdfinteractionstate_p.h"

#include "pdfdocumentbuilder.h"

#include <QtTest>

class InteractionStateTest final : public QObject
{
    Q_OBJECT

private slots:
    void transitionsRemainTransient();
    void sameInteractionReusesToken();
    void revisionConflictRejectsContinuation();
    void cacheGenerationConflictRejectsContinuation();
    void cancellationIsTerminalAndHoverCanClearIndependently();
};

namespace
{

pdf::PDFDocument makeDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    return builder.build();
}

}

void InteractionStateTest::transitionsRemainTransient()
{
    pdf::PDFDocument document = makeDocument();
    pdf::PDFDocumentContext context(&document);
    const pdf::PDFRevisionIdentity before = context.getRevision();

    pdf::PDFInteractionState state;
    const pdf::PDFInteractionState::Token drag = state.begin(pdf::PDFInteractionState::Kind::Drag, before);
    QVERIFY(state.isCurrent(drag, before));
    QVERIFY(state.update(drag, before));
    QVERIFY(state.complete(drag, before));
    QCOMPARE(context.getRevision(), before);

    const pdf::PDFInteractionState::Token zoom = state.begin(pdf::PDFInteractionState::Kind::ZoomPan, before);
    QVERIFY(state.isCurrent(zoom, before));
    QVERIFY(state.complete(zoom, before));
    QCOMPARE(context.getRevision(), before);
}

void InteractionStateTest::sameInteractionReusesToken()
{
    pdf::PDFInteractionState state;
    const pdf::PDFRevisionIdentity revision;

    const pdf::PDFInteractionState::Token first = state.begin(pdf::PDFInteractionState::Kind::Hover, revision);
    const pdf::PDFInteractionState::Token second = state.begin(pdf::PDFInteractionState::Kind::Hover, revision);
    QCOMPARE(second.generation, first.generation);
    QCOMPARE(second.revision, first.revision);

    const pdf::PDFInteractionState::Token changed = state.begin(pdf::PDFInteractionState::Kind::Drag, revision);
    QVERIFY(changed.generation != first.generation);
    QVERIFY(!state.isCurrent(first, revision));
}

void InteractionStateTest::revisionConflictRejectsContinuation()
{
    pdf::PDFDocument document = makeDocument();
    pdf::PDFDocumentContext context(&document);
    const pdf::PDFRevisionIdentity firstRevision = context.getRevision();

    pdf::PDFInteractionState state;
    const pdf::PDFInteractionState::Token token = state.begin(pdf::PDFInteractionState::Kind::ToolGesture, firstRevision);
    context.markModified(pdf::PDFModifiedDocument::PageContents);

    QVERIFY(!state.isCurrent(token, context.getRevision()));
    QVERIFY(!state.complete(token, context.getRevision()));
    state.cancel(pdf::PDFInteractionState::CancelReason::RevisionChanged);

    const pdf::PDFInteractionState::Snapshot snapshot = state.snapshot();
    QVERIFY(!snapshot.active());
    QCOMPARE(snapshot.lastCancelReason, pdf::PDFInteractionState::CancelReason::RevisionChanged);
}

void InteractionStateTest::cacheGenerationConflictRejectsContinuation()
{
    pdf::PDFDocument document = makeDocument();
    pdf::PDFDocumentContext context(&document);
    const pdf::PDFRevisionIdentity firstRevision = context.getRevision();

    pdf::PDFInteractionState state;
    const pdf::PDFInteractionState::Token token = state.begin(pdf::PDFInteractionState::Kind::ZoomPan, firstRevision);
    context.invalidateCaches();

    QVERIFY(!state.isCurrent(token, context.getRevision()));
    state.cancel(pdf::PDFInteractionState::CancelReason::RevisionChanged);
    QVERIFY(!state.snapshot().active());
}

void InteractionStateTest::cancellationIsTerminalAndHoverCanClearIndependently()
{
    pdf::PDFInteractionState state;
    const pdf::PDFRevisionIdentity revision;

    const pdf::PDFInteractionState::Token gesture = state.begin(pdf::PDFInteractionState::Kind::Marquee, revision);
    state.cancel(pdf::PDFInteractionState::CancelReason::Escape);
    QVERIFY(!state.snapshot().active());
    QCOMPARE(state.snapshot().lastCancelReason, pdf::PDFInteractionState::CancelReason::Escape);
    QVERIFY(!state.complete(gesture, revision));

    const pdf::PDFInteractionState::Token hover = state.begin(pdf::PDFInteractionState::Kind::Hover, revision);
    QVERIFY(state.isCurrent(hover, revision));
    state.clearHover();
    QVERIFY(!state.snapshot().active());
}

QTEST_APPLESS_MAIN(InteractionStateTest)

#include "tst_interactionstatetest.moc"
