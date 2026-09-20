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

#ifndef EDITORHOST_H
#define EDITORHOST_H

#include "interactionstate.h"

#include "commandcatalog.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "findingnavigation.h"
#include "hittestsource.h"
#include "inspectormodel.h"
#include "jobsubmitter.h"
#include "pagesurfacerenderer.h"
#include "actionlistcatalog.h"
#include "actionlistcontroller.h"
#include "actionlistrunsubmitter.h"
#include "preflightcontroller.h"
#include "preflightoverlaybridge.h"
#include "preflightprofiledraft.h"
#include "previewstatemodel.h"
#include "productionmodel.h"
#include "viewportcommandbridge.h"
#include "viewportcontroller.h"

#include "focusrestoration.h"
#include "documentviewsession.h"
#include "quickdocumentmodel.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfoperationhistory.h"

#include <QColor>
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QStyleHints>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace pdfinteraction
{
class HitTestDispatcher;
class InteractionController;
class OverlayBuilder;
}   // namespace pdfinteraction

namespace pdfquick
{
class LoopCanvasItem;
}   // namespace pdfquick

/// C++ presentation host for the packaged Loop.Quick shell (P4-S7).
///
/// Owns the one document context, scheduler adapter, command catalog, lifecycle
/// facade, viewport, surfaces, and interaction stack. QML sees presentation
/// properties and catalog invoke() only; Core objects never cross the boundary.
class EditorHost final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString documentState READ documentState NOTIFY presentationChanged)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY presentationChanged)
    Q_PROPERTY(QString displayTitle READ displayTitle NOTIFY presentationChanged)
    Q_PROPERTY(QString typedError READ typedError NOTIFY presentationChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY presentationChanged)
    Q_PROPERTY(int currentPage READ currentPage NOTIFY presentationChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY presentationChanged)
    Q_PROPERTY(int rotationDegrees READ rotationDegrees NOTIFY presentationChanged)
    Q_PROPERTY(bool incomplete READ incomplete NOTIFY presentationChanged)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY presentationChanged)
    Q_PROPERTY(bool unsupported READ unsupported NOTIFY presentationChanged)
    Q_PROPERTY(int commandEpoch READ commandEpoch NOTIFY commandEpochChanged)
    Q_PROPERTY(QObject* preflight READ preflight CONSTANT)
    Q_PROPERTY(QObject* actionList READ actionList CONSTANT)
    Q_PROPERTY(QObject* inspector READ inspector CONSTANT)
    Q_PROPERTY(QObject* preview READ preview CONSTANT)
    Q_PROPERTY(QObject* documentModel READ documentModel CONSTANT)
    Q_PROPERTY(QObject* focusRestoration READ focusRestoration CONSTANT)
    Q_PROPERTY(QString preflightStateName READ preflightStateName NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap preflightStateVisual READ preflightStateVisual NOTIFY presentationChanged)
    Q_PROPERTY(QColor preflightStateColor READ preflightStateColor NOTIFY presentationChanged)
    Q_PROPERTY(QString preflightOperatorSummary READ preflightOperatorSummary NOTIFY presentationChanged)
    Q_PROPERTY(QString preflightCertificateStateName READ preflightCertificateStateName NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap preflightCertificateStateVisual READ preflightCertificateStateVisual NOTIFY presentationChanged)
    Q_PROPERTY(QColor preflightCertificateStateColor READ preflightCertificateStateColor NOTIFY presentationChanged)
    Q_PROPERTY(QString preflightCertificateSummary READ preflightCertificateSummary NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList preflightProfiles READ preflightProfiles NOTIFY preflightProfilesChanged)
    Q_PROPERTY(QVariantList preflightVariables READ preflightVariables NOTIFY preflightProfilesChanged)
    Q_PROPERTY(QString selectedPreflightProfileId READ selectedPreflightProfileId NOTIFY preflightProfilesChanged)
    Q_PROPERTY(bool preflightProfileEditing READ preflightProfileEditing NOTIFY preflightProfileDraftChanged)
    Q_PROPERTY(QVariantList preflightEditableChecks READ preflightEditableChecks NOTIFY preflightProfileDraftChanged)
    Q_PROPERTY(QString preflightProfileDraftVersion READ preflightProfileDraftVersion NOTIFY preflightProfileDraftChanged)
    Q_PROPERTY(QVariantList actionListRecipes READ actionListRecipes NOTIFY actionListRecipesChanged)
    Q_PROPERTY(QString selectedActionListRecipeId READ selectedActionListRecipeId NOTIFY actionListRecipesChanged)
    Q_PROPERTY(QVariantList actionListBindings READ actionListBindings NOTIFY actionListRecipesChanged)
    Q_PROPERTY(QVariantList actionListSteps READ actionListSteps NOTIFY actionListRecipesChanged)
    Q_PROPERTY(QString actionListStateName READ actionListStateName NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList repairOperations READ repairOperations CONSTANT)
    Q_PROPERTY(bool hasPreflightReport READ hasPreflightReport NOTIFY presentationChanged)
    Q_PROPERTY(QString previewSummary READ previewSummary NOTIFY presentationChanged)
    Q_PROPERTY(QString inspectorTitle READ inspectorTitle NOTIFY presentationChanged)
    Q_PROPERTY(QString inspectionMode READ inspectionMode NOTIFY presentationChanged)
    Q_PROPERTY(bool preferReducedMotion READ preferReducedMotion NOTIFY presentationChanged)
    Q_PROPERTY(bool highContrast READ highContrast NOTIFY presentationChanged)
    Q_PROPERTY(bool pageFidelityIsExact READ pageFidelityIsExact NOTIFY presentationChanged)
    Q_PROPERTY(QString pageFidelityReason READ pageFidelityReason NOTIFY presentationChanged)
    Q_PROPERTY(bool pageFidelityIsAuthoritative READ pageFidelityIsAuthoritative NOTIFY presentationChanged)
    Q_PROPERTY(bool searchPanelVisible READ searchPanelVisible NOTIFY presentationChanged)
    Q_PROPERTY(bool fullscreenRequested READ fullscreenRequested NOTIFY presentationChanged)
    Q_PROPERTY(int workspaceRequest READ workspaceRequest NOTIFY presentationChanged)
    Q_PROPERTY(LoopWorkspace workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
    Q_PROPERTY(QString documentShellStatus READ documentShellStatus NOTIFY presentationChanged)
    Q_PROPERTY(QString productionStateName READ productionStateName NOTIFY presentationChanged)
    Q_PROPERTY(bool allowDeveloperDiagnostics READ allowDeveloperDiagnostics CONSTANT)

    // ---- Workspace surfaces (#586) -------------------------------------------------
    // Every property below is a read-only projection of what Core, PdfTool or the
    // shell's own run already decided. QML renders these; it derives no state and
    // calls no mutator, writer, artifact store or PDFRepairTransaction::apply().
    Q_PROPERTY(QObject* production READ production CONSTANT)
    Q_PROPERTY(QString fixLifecycleStateName READ fixLifecycleStateName NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap fixLifecycleVisual READ fixLifecycleVisual NOTIFY presentationChanged)
    Q_PROPERTY(QColor fixLifecycleColor READ fixLifecycleColor NOTIFY presentationChanged)
    Q_PROPERTY(QString fixLifecycleSummary READ fixLifecycleSummary NOTIFY presentationChanged)
    Q_PROPERTY(bool fixExecutionArmed READ fixExecutionArmed NOTIFY presentationChanged)
    Q_PROPERTY(bool fixRollbackAvailable READ fixRollbackAvailable NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap fixPlanIdentity READ fixPlanIdentity NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap fixPreview READ fixPreview NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap fixRecheck READ fixRecheck NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap fixSignOff READ fixSignOff NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList fixRollbackPoints READ fixRollbackPoints NOTIFY presentationChanged)
    Q_PROPERTY(QString fixRollbackSummary READ fixRollbackSummary NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap previewIdentity READ previewIdentity NOTIFY presentationChanged)
    Q_PROPERTY(QString previewStaleReason READ previewStaleReason NOTIFY presentationChanged)

public:
    enum LoopWorkspace
    {
        Document = 0,
        Preflight = 1,
        ProductionPreview = 2,
        Pages = 3,
        Inspect = 4,
        Fix = 5,
        Compare = 6
    };
    Q_ENUM(LoopWorkspace)

    explicit EditorHost(QObject* parent = nullptr);
    ~EditorHost() override;

    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    QString documentState() const;
    bool hasDocument() const;
    QString displayTitle() const;
    QString typedError() const;
    int pageCount() const;
    int currentPage() const;
    qreal zoom() const;
    int rotationDegrees() const;
    bool incomplete() const;
    bool cancelled() const;
    bool unsupported() const;
    int commandEpoch() const noexcept { return m_commandEpoch; }

    QObject* preflight();
    QObject* actionList();
    QObject* inspector();
    QObject* preview();
    QObject* documentModel() { return &m_documentModel; }
    FocusRestoration* focusRestoration() { return &m_focusRestoration; }

    QString preflightStateName() const;

    /// Canonical #194 treatment for the current document-level preflight state: the keys
    /// `kind`, `colorRole`, `icon` and `accessibleName`, all computed by LoopLibQuick from Core's
    /// own state name. QML renders it; it derives nothing and picks no roles.
    QVariantMap preflightStateVisual() const;

    /// The `colorRole` above, resolved to a colour for the current theme. QML must never map a role
    /// name to a colour itself.
    QColor preflightStateColor() const;

    QString preflightOperatorSummary() const;
    QString preflightCertificateStateName() const noexcept { return m_preflightCertificateStateName; }
    QVariantMap preflightCertificateStateVisual() const;
    QColor preflightCertificateStateColor() const;
    QString preflightCertificateSummary() const noexcept { return m_preflightCertificateSummary; }
    QVariantList preflightProfiles() const;
    QVariantList preflightVariables() const;
    QString selectedPreflightProfileId() const;
    bool preflightProfileEditing() const;
    QVariantList preflightEditableChecks() const;
    QString preflightProfileDraftVersion() const;
    QVariantList actionListRecipes() const;
    QString selectedActionListRecipeId() const;
    QVariantList actionListBindings() const;
    QVariantList actionListSteps() const;
    QString actionListStateName() const;
    QVariantList repairOperations() const;
    bool hasPreflightReport() const noexcept { return m_preflight.hasResult(); }
    QString previewSummary() const;
    QString inspectorTitle() const;
    QString inspectionMode() const noexcept { return m_inspectionMode; }
    bool preferReducedMotion() const;
    bool highContrast() const;
    bool searchPanelVisible() const noexcept { return m_searchPanelVisible; }
    bool fullscreenRequested() const noexcept { return m_fullscreenRequested; }
    int workspaceRequest() const noexcept { return m_workspaceRequest; }
    LoopWorkspace workspace() const noexcept { return m_workspace; }
    QString documentShellStatus() const;
    QString productionStateName() const;
    bool allowDeveloperDiagnostics() const;

    /// Production / plate model for the Pages and Production Preview surfaces.
    QObject* production() { return &m_production; }

    /// Operator lifecycle of the governed-correction route (#586):
    /// `idle`, `planned`, `preview-ready`, `approved`, `executing`, `succeeded`,
    /// `stale`, `rejected`, `cancelled`, `failed`. It is a projection of the run the
    /// shell already owns (`ActionListController`) plus the operator's own review
    /// decision, never a second approval authority: the publication approval is Core's
    /// plan-bound approval from `ActionListRunSubmitter`.
    QString fixLifecycleStateName() const;
    QVariantMap fixLifecycleVisual() const;
    QColor fixLifecycleColor() const;
    QString fixLifecycleSummary() const;

    /// True when the reviewed plan digest is approved for the current revision, which is
    /// the only condition under which the shell will execute it.
    bool fixExecutionArmed() const;

    /// True when the open document has recorded rollback points the operator may return to.
    bool fixRollbackAvailable() const;

    /// Exact identities every Fix affordance is bound to: document key/revision, source
    /// digest, recipe hash, plan digest, effective profile digest and the review decision.
    QVariantMap fixPlanIdentity() const;

    /// Technical/visual preview of the planned or executed candidate, carrying source and
    /// candidate digests, the plan digest and its incomplete/fidelity state.
    QVariantMap fixPreview() const;

    /// Recheck evidence after a run: postflight outcome and the finding delta.
    QVariantMap fixRecheck() const;

    /// Publication sign-off: Core's governed status plus the certified-preflight state.
    QVariantMap fixSignOff() const;

    /// Recorded rollback points for the open document (read-only).
    QVariantList fixRollbackPoints() const;

    /// Why the rollback affordance is enabled or unavailable, in the operator's words.
    QString fixRollbackSummary() const;

    /// Identity of the production preview currently on screen. `previewIdentity.stale`
    /// and `previewStaleReason` say whether it still describes the open revision.
    QVariantMap previewIdentity() const;
    QString previewStaleReason() const;

    /// Overprint render fidelity for the currently displayed page (issue #49).
    /// True (and pageFidelityReason empty) when the page has no overprint
    /// content, or none is known yet. Separate from the document-wide
    /// PreviewStateModel exposed as `preview`: this is per-page and driven by
    /// the cached PDFPrecompiledPage::containsOverprint() flag / the
    /// authoritative renderer's own diagnostics, not a static message.
    bool pageFidelityIsExact() const;
    QString pageFidelityReason() const;

    /// True while the current page is showing the escalated
    /// PDFRenderPolicy::forOutputPreview() render instead of the fast
    /// approximate one.
    bool pageFidelityIsAuthoritative() const;

    Q_INVOKABLE void selectFinding(const QString& findingId);
    Q_INVOKABLE bool selectNextFinding();
    Q_INVOKABLE bool selectPreviousFinding();
    Q_INVOKABLE void announceDocumentState(const QString& message);
    Q_INVOKABLE bool runPreflight();
    Q_INVOKABLE bool cancelPreflight();
    Q_INVOKABLE bool selectPreflightProfile(const QString& id);
    Q_INVOKABLE bool setPreflightVariable(const QString& name, const QVariant& value);
    Q_INVOKABLE void requestPreflightReportExport();
    Q_INVOKABLE bool exportPreflightReportFileUrl(const QUrl& url);
    Q_INVOKABLE void requestPreflightProfileImport();
    Q_INVOKABLE void requestPreflightProfileExport();
    Q_INVOKABLE void requestPreflightProfileSave();
    Q_INVOKABLE bool importPreflightProfileFileUrl(const QUrl& url);
    Q_INVOKABLE bool beginPreflightProfileEdit();
    Q_INVOKABLE bool setPreflightCheckField(const QString& checkId, const QString& field, const QVariant& value);
    Q_INVOKABLE bool savePreflightProfileEdit(const QString& newVersion, const QUrl& url);
    Q_INVOKABLE bool exportPreflightProfileFileUrl(const QUrl& url);
    Q_INVOKABLE void cancelPreflightProfileEdit();
    Q_INVOKABLE bool importActionListRecipe(const QUrl& url);
    Q_INVOKABLE bool exportActionListRecipe(const QUrl& url);
    Q_INVOKABLE bool selectActionListRecipe(const QString& id);
    Q_INVOKABLE bool setActionListBinding(const QString& name, const QVariant& value);
    Q_INVOKABLE bool setActionListStepParameter(int stepIndex, const QString& name, const QVariant& value);
    Q_INVOKABLE bool saveActionListRecipe();
    Q_INVOKABLE bool validateActionListRecipe();
    Q_INVOKABLE bool planActionList();
    Q_INVOKABLE bool runActionList();
    Q_INVOKABLE bool cancelActionList();
    Q_INVOKABLE bool confirmActionListPlan();
    Q_INVOKABLE void discardActionListPlan();
    Q_INVOKABLE QVariantMap repairParameterSchemaForOperation(const QString& operationId) const;

    // ---- Governed-correction review intents (#586) --------------------------------
    /// The operator's review of the planned correction. Approving binds the decision to
    /// the exact plan digest for the current revision and arms execution; a plan whose
    /// revision or digest moved is never armed. Rejecting records the decision and leaves
    /// the plan visible for inspection; replanning clears both.
    Q_INVOKABLE bool approveActionListPlan();
    Q_INVOKABLE bool rejectActionListPlan();
    Q_INVOKABLE bool executeApprovedActionListPlan();
    Q_INVOKABLE void replanActionList();

    /// Selects a planned or executed step so the Inspect workspace shows that
    /// operation's typed facts (identity, target, parameters, impact, save policy,
    /// unresolved risk, approval state, output identity, provenance).
    Q_INVOKABLE bool inspectActionListStep(int stepIndex);

    /// Returns the open document to a recorded rollback point. Core writes a new
    /// revision and appends a rolled-back event; existing history is never rewritten.
    Q_INVOKABLE bool requestFixRollback(const QString& rollbackId);
    Q_INVOKABLE void refreshFixRollbackPoints();

    /// Toggles the current page between the fast approximate render and the
    /// authoritative overprint-accurate one. Re-renders only that page;
    /// the document stays open.
    Q_INVOKABLE void toggleCurrentPageFidelity();
    Q_INVOKABLE void goToPage(int pageIndex);
    Q_INVOKABLE void goToOutlinePage(int pageIndex);
    Q_INVOKABLE void setWorkspace(LoopWorkspace workspace);

    /// Selects a recipe that runs `operationId` and routes to the Fix workspace, so the
    /// Inspect workspace's "plan a fix for this finding" intent lands somewhere that can
    /// actually plan it. Nothing is planned, approved or executed here.
    Q_INVOKABLE bool selectActionListRecipeForOperation(const QString& operationId);

    /// The Inspect workspace's corrective intent: selects and binds, then stops.
    void onCorrectiveOperationRequested(const pdfinteraction::InspectorCorrectiveOperationIntent& intent);
    /// Compare remains a visible but disabled destination until its product
    /// decision is approved. This check is shared by QML and C++ callers so a
    /// non-QML caller cannot bypass the shell routing policy.
    Q_INVOKABLE bool isWorkspaceEnabled(LoopWorkspace workspace) const;
    Q_INVOKABLE void acknowledgeWorkspaceRequest();
    Q_INVOKABLE void acknowledgeSearchPanel();

    Q_INVOKABLE QVariantList commandDescriptors() const;
    Q_INVOKABLE bool isCommandEnabled(const QString& commandId) const;
    Q_INVOKABLE quint64 invokeCommand(const QString& commandId, const QVariantMap& parameters = {});
    Q_INVOKABLE bool cancelCommand(quint64 invocationId);

    /// QML FileDialog passes a file URL; paths stay in C++.
    Q_INVOKABLE void openFileUrl(const QUrl& url);
    Q_INVOKABLE void saveAsFileUrl(const QUrl& url);
    Q_INVOKABLE void reopenDocument();
    Q_INVOKABLE void cancelPendingOperation();

    /// Observed LoopCanvas item from CanvasPane.qml. Binding lifetime is owned
    /// here: replace/close unbinds before geometry is dropped.
    Q_INVOKABLE void attachCanvas(QObject* canvasObject);
    Q_INVOKABLE void detachCanvas();

    /// Legacy geometry hook for headless tests without a LoopCanvas item.
    Q_INVOKABLE void setViewportGeometry(qreal pixelPerMM, qreal devicePixelRatio, int widthPx, int heightPx);

    /// Opens a positional CLI path after the shell is loaded (C++-only).
    void openInitialPath(const QString& path);
    Q_INVOKABLE QString shortcutForCommand(const QString& commandId) const;

    // Test-only accessor for large-document shell stress parity (gh-363).
    // Exposes the privately owned DocumentViewSession so headless shell tests
    // can observe revision fencing (revisionSource), viewport generation
    // (viewport().requestGeneration()), surface demand (surfaces()->counters())
    // and interaction overlay fencing (interaction()->overlayFrameChanged())
    // without breaking QML encapsulation. Not part of the QML API.
    DocumentViewSession* sessionForTest() noexcept { return m_session.get(); }
    const DocumentViewSession* sessionForTest() const noexcept { return m_session.get(); }

signals:
    void presentationChanged();
    void commandEpochChanged();
    void workspaceChanged(LoopWorkspace from, LoopWorkspace to);
    void preflightProfilesChanged();
    void preflightProfileDraftChanged();
    void actionListRecipesChanged();
    void preflightReportExportRequested();
    void preflightProfileImportRequested();
    void preflightProfileExportRequested();
    void preflightProfileSaveRequested();

private:
    void connectFacade();
    void connectViewport();
    void connectCatalog();
    void connectInteraction();
    void connectSurfaces();
    void registerShellHandlers();
    void registerFeatureHandlers();
    void refreshFeatureAvailability();
    void moveSearch(int direction);
    bool moveFindingSelection(int direction);
    void refreshHitTestSources();
    void bumpPresentation();
    void bumpCommandEpoch();

    void onDocumentReady();
    void onDocumentGone();
    void syncDocumentLifecycle();
    void bindCanvas();
    void unbindCanvas();
    QStringList activeAsyncWorkKinds() const;
    void acceptPreflightResult(const QString& jobId,
                               const QString& documentRevision,
                               const pdf::PreflightResult& result);
    void finishPreflightJob(const pdf::PDFJobSnapshot& snapshot);
    void finishActionListJob(const pdf::PDFJobSnapshot& snapshot);
    bool submitActionListJob(pdfinteraction::ActionListRunPhase phase,
                             pdfinteraction::ActionListController::State controllerState);
    void reloadActionListRecipes();
    void updateActionListRecipeWatch();
    void syncActionListDraft();
    void refreshCanvasTrace();
    void refreshPreflightCertificateState();
    void reloadPreflightProfiles();
    void updatePreflightProfileWatch();
    void syncRevisionModels();
    void updateCanvasAccessibilitySummary();
    void onPreflightNavigation(pdfinteraction::PreflightController::EvidenceNavigationRequest request);
    void onDragCompleted(pdfinteraction::DragSession session);
    void onInteractionSelectionChanged(pdfinteraction::InteractionTarget target);
    void syncProductionState();
    void applyInspectorSelection(const pdfinteraction::InteractionTarget& target);
    void applyEmptyCanvasInspectorSelection();
    void setInspectionMode(QString mode);

    /// Operator review of the planned correction (#586). `fixPlanIsCurrent()` is the only
    /// authority on whether the reviewed plan still describes the open revision.
    QString fixCurrentPlanDigest() const;
    bool fixPlanIsCurrent() const;
    /// True when the operator had a plan for a different revision than the one now open.
    bool fixPlannedPlanIsSuperseded() const;
    void clearFixReview();
    pdf::PDFActionListExecutionResult fixRunResult() const;
    QVariantMap fixPreviewFromResult() const;
    QList<pdf::PDFRollbackPoint> documentRollbackPoints() const;
    void refreshFixRollbackPointsFromHistory();

    std::unique_ptr<DocumentViewSession> m_session;
    std::unique_ptr<pdfinteraction::FindingCanvasNavigator> m_findingNavigator;
    pdfinteraction::PreflightController m_preflight;
    pdfinteraction::ActionListCatalog m_actionListCatalog;
    pdfinteraction::ActionListController m_actionListController;
    pdfinteraction::PreflightOverlayBridge m_preflightOverlayBridge;
    pdfinteraction::InspectorModel m_inspector;
    pdfinteraction::PreviewStateModel m_preview;
    pdfinteraction::ProductionModel m_production;
    QuickDocumentModel m_documentModel;
    FocusRestoration m_focusRestoration;
    pdfinteraction::FindingListHitTestSource m_findingsHitTest;

    QPointer<pdfquick::LoopCanvasItem> m_canvas;
    QHash<QString, pdf::PDFJobKind> m_activeAsyncJobs;
    struct PreflightWorkerOutcome;
    QHash<QString, std::shared_ptr<PreflightWorkerOutcome>> m_preflightOutcomes;
    QHash<QString, std::shared_ptr<pdfinteraction::ActionListWorkerOutcome>> m_actionListOutcomes;
    struct PreflightProfileChoice
    {
        QString id;
        QString name;
        QString version;
        QString source;
        QString diagnostic;
        QString digest;
        QJsonObject profile;
        QJsonObject variables;
        bool valid = false;
    };
    QList<PreflightProfileChoice> m_preflightProfiles;
    QJsonObject m_preflightBindings;
    QString m_selectedPreflightProfileId;
    pdfinteraction::PreflightProfileDraft m_preflightProfileDraft;
    class QFileSystemWatcher* m_preflightProfileWatcher = nullptr;
    class QFileSystemWatcher* m_actionListRecipeWatcher = nullptr;
    bool m_acceptPreflightResults = true;
    bool m_acceptActionListResults = true;
    QString m_selectedActionListRecipeId;
    QJsonObject m_actionListBindings;
    pdf::PDFActionList m_actionListDraft;
    bool m_actionListDraftValid = false;
    int m_commandEpoch = 0;
    bool m_documentBound = false;
    bool m_searchPanelVisible = false;
    bool m_fullscreenRequested = false;
    int m_workspaceRequest = -1;
    LoopWorkspace m_workspace = LoopWorkspace::Document;
    int m_searchRow = -1;
    QString m_inspectionMode = QStringLiteral("page");
    QString m_preflightCertificateStateName = QStringLiteral("not-certified");
    QString m_preflightCertificateSummary = QStringLiteral("No certified preflight is recorded for this document.");

    /// Review decision for the governed-correction route (#586). The digest records which
    /// plan the operator reviewed, so a replan or a revision change cannot inherit it.
    enum class FixReviewDecision
    {
        None,
        Approved,
        Rejected
    };
    FixReviewDecision m_fixReviewDecision = FixReviewDecision::None;
    QString m_fixReviewedPlanDigest;

    /// The plan the operator last had, and the document revision it was produced for. The
    /// reviewed digest and every Fix affordance are bound to them, so nothing can be presented
    /// as current evidence for a revision it does not describe. They outlive the run controller
    /// so a superseded plan is reported as `stale` rather than silently disappearing.
    QString m_fixPlannedDocumentRevision;
    QString m_fixPlannedPlanDigest;
    QVariantList m_fixRollbackPoints;
    QString m_fixRollbackSummary;
};

#endif   // EDITORHOST_H
