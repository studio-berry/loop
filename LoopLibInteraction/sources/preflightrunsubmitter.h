#ifndef PREFLIGHTRUNSUBMITTER_H
#define PREFLIGHTRUNSUBMITTER_H

#include "preflightprofilecatalog.h"

#include "pdfdocument.h"
#include "pdfjobscheduler.h"
#include "preflightengine.h"

#include <functional>
#include <memory>

namespace pdfinteraction
{

struct PreflightRunOutcome
{
    pdf::PreflightResult result;
};

struct PreflightRunRequest
{
    pdf::PDFDocumentPointer document;
    PreflightProfileChoice profile;
    QJsonObject bindings;
    QByteArray sourceHash;
};

/// Builds the scheduler worker for an in-memory preflight run. Keeps Core
/// orchestration out of EditorHost while reusing the same import/bind/parse/run
/// path as the CLI adapter.
std::function<void(pdf::PDFJobContext&)> makePreflightRunWorker(const PreflightRunRequest& request, const std::shared_ptr<PreflightRunOutcome>& outcome);

}   // namespace pdfinteraction

#endif   // PREFLIGHTRUNSUBMITTER_H
