#include "shellinspectordispatch.h"

namespace pdfinteraction
{

InspectorModel::Selection buildEmptyCanvasInspectorSelection(const ShellInspectorContext& context)
{
    InspectorModel::Selection selection;
    selection.documentKey = context.documentKey;
    selection.documentRevision = context.documentRevision;
    selection.selectionId = QStringLiteral("canvas");
    selection.title = context.canvasTitle;
    selection.kind = InspectorModel::SelectionKind::EmptyCanvas;
    selection.properties = {
        { QStringLiteral("document"), QStringLiteral("Document"), context.displayTitle },
        { QStringLiteral("pages"), QStringLiteral("Pages"), QString::number(context.pageCount) },
        { QStringLiteral("document-status"), QStringLiteral("Document status"), context.documentShellStatus },
        { QStringLiteral("preflight"), QStringLiteral("Preflight"), context.preflightStateName },
        { QStringLiteral("production"), QStringLiteral("Production"), context.productionStateName },
    };
    return selection;
}

InspectorModel::Selection buildInspectorSelection(const InteractionTarget& target,
                                                  const ShellInspectorContext& context)
{
    if (!target.isValid())
    {
        return buildEmptyCanvasInspectorSelection(context);
    }

    if (target.id.startsWith(QStringLiteral("image:")))
    {
        InspectorModel::Selection selection;
        selection.documentKey = context.documentKey;
        selection.documentRevision = context.documentRevision;
        selection.selectionId = target.id;
        selection.title = context.imageTitle;
        selection.kind = InspectorModel::SelectionKind::Image;
        selection.properties = {
            { QStringLiteral("id"), QStringLiteral("Image"), target.id.mid(6) },
            { QStringLiteral("page"), QStringLiteral("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("bounds"), QStringLiteral("Bounds"),
              QStringLiteral("%1,%2 %3x%4")
                  .arg(QString::number(target.pageBounds.x()),
                       QString::number(target.pageBounds.y()),
                       QString::number(target.pageBounds.width()),
                       QString::number(target.pageBounds.height())) },
        };
        return selection;
    }

    if (target.id.startsWith(QStringLiteral("separation:")))
    {
        InspectorModel::Selection selection;
        selection.documentKey = context.documentKey;
        selection.documentRevision = context.documentRevision;
        selection.selectionId = target.id;
        selection.title = context.separationTitle;
        selection.kind = InspectorModel::SelectionKind::Separation;
        selection.properties = {
            { QStringLiteral("name"), QStringLiteral("Ink"), target.id.mid(11) },
            { QStringLiteral("page"), QStringLiteral("Page"), QString::number(target.pageIndex + 1) },
        };
        return selection;
    }

    if (target.kind == InteractionTargetKind::Page || target.kind == InteractionTargetKind::PageBox)
    {
        InspectorModel::Selection selection;
        selection.documentKey = context.documentKey;
        selection.documentRevision = context.documentRevision;
        selection.selectionId = target.id.isEmpty() ? QStringLiteral("page") : target.id;
        selection.title = target.kind == InteractionTargetKind::PageBox
                              ? context.pageBoxTitlePrefix.arg(target.id)
                              : context.pageTitlePrefix.arg(target.pageIndex + 1);
        selection.kind = InspectorModel::SelectionKind::Page;
        selection.properties = {
            { QStringLiteral("page"), QStringLiteral("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("box"), QStringLiteral("Box"), target.id },
            { QStringLiteral("size"), QStringLiteral("Size"),
              QStringLiteral("%1 x %2")
                  .arg(QString::number(target.pageBounds.width()), QString::number(target.pageBounds.height())) },
            { QStringLiteral("rotation"), QStringLiteral("Rotation"), QStringLiteral("%1°").arg(context.rotationDegrees) },
            { QStringLiteral("ocg"), QStringLiteral("Optional content"),
              context.hasOptionalContent ? context.optionalContentPresent : context.optionalContentNone },
        };
        return selection;
    }

    return buildEmptyCanvasInspectorSelection(context);
}

}   // namespace pdfinteraction
