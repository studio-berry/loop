#ifndef SHELLINSPECTORDISPATCH_H
#define SHELLINSPECTORDISPATCH_H

#include "inspectormodel.h"
#include "interactiontarget.h"

namespace pdfinteraction
{

struct ShellInspectorContext
{
    QString documentKey;
    QString documentRevision;
    QString displayTitle;
    int pageCount = 0;
    QString documentShellStatus;
    QString preflightStateName;
    QString productionStateName;
    int rotationDegrees = 0;
    bool hasOptionalContent = false;
    QString canvasTitle;
    QString imageTitle;
    QString separationTitle;
    QString pageTitlePrefix;
    QString pageBoxTitlePrefix;
    QString optionalContentPresent;
    QString optionalContentNone;
};

/// Builds inspector selections from shell interaction targets without keeping
/// dispatch logic in EditorHost.
InspectorModel::Selection buildEmptyCanvasInspectorSelection(const ShellInspectorContext& context);
InspectorModel::Selection buildInspectorSelection(const InteractionTarget& target,
                                                  const ShellInspectorContext& context);

}   // namespace pdfinteraction

#endif   // SHELLINSPECTORDISPATCH_H
