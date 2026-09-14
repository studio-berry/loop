#include "preflightrunsubmitter.h"

#include "preflightclirun.h"

#include "pdfdocumentsession.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"

#include <stdexcept>

namespace pdfinteraction
{

std::function<void(pdf::PDFJobContext&)> makePreflightRunWorker(const PreflightRunRequest& request,
                                                                 const std::shared_ptr<PreflightRunOutcome>& outcome)
{
    return [document = request.document, outcome, profile = request.profile, bindings = request.bindings,
            sourceHash = request.sourceHash](pdf::PDFJobContext& context)
    {
        if (context.isCancellationRequested())
        {
            return;
        }

        const pdf::PreflightProfileImportResult imported =
            pdf::importPreflightProfile(profile.profile, profile.source);
        if (!imported.ok)
        {
            throw std::runtime_error(imported.errorMessage.toStdString());
        }
        const pdf::PreflightVariableBindResult bound = pdf::bindPreflightProfileVariables(imported.profile, bindings);
        if (!bound.ok)
        {
            throw std::runtime_error(bound.errorMessage.toStdString());
        }
        pdf::PreflightProfileResolver resolver;
        const pdf::PreflightResolvedProfile resolved = resolver.resolveExplicitProfile(
            bound.profile, profile.name,
            imported.identity.version.isEmpty() ? QStringLiteral("explicit") : imported.identity.version);
        if (!resolved.ok)
        {
            throw std::runtime_error(resolved.errorMessage.toStdString());
        }

        pdf::PreflightProfileData profileData;
        QString profileError;
        if (!pdf::PreflightEngine::parseProfile(bound.profile, profileData, profileError))
        {
            throw std::runtime_error(profileError.toStdString());
        }
        profileData.variableBindings = bound.bindings;
        profileData.fileDigest = imported.identity.digest;
        profileData.effectiveDigest = pdf::computeProfileDigest(bound.profile);
        profileData.profileIdentity = imported.identity.toJson();
        profileData.profileIdentity.insert(QStringLiteral("effective_digest"), profileData.effectiveDigest);
        context.reportProgress(5);

        std::unique_ptr<pdf::PDFDocumentSession, void (*)(pdf::PDFDocumentSession*)> session(
            pdf::PDFDocumentSession::createForInspection(document.data()), &pdf::PDFDocumentSession::destroy);
        pdf::PreflightEngine engine(session.get());
        engine.setOperationControl(context.operationControl());
        context.reportProgress(15);
        outcome->result = engine.run(profileData);
        pdf::finalizePreflightResult(outcome->result, sourceHash, resolved);
        if (context.isCancellationRequested())
        {
            return;
        }
        context.reportProgress(95);
        context.setResultSummary(QStringLiteral("Preflight completed."));
    };
}

}   // namespace pdfinteraction
