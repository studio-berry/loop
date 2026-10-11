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
#include "editorsettings.h"
#include "quickdocumentmodel.h"

#include "canvasdisplaycolor.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfoperationhistory.h"

#include <QColor>
#include <QImage>
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QStyleHints>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

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
    Q_PROPERTY(bool correctionsAllowed READ correctionsAllowed NOTIFY presentationChanged)
    Q_PROPERTY(QString displayTitle READ displayTitle NOTIFY presentationChanged)
    Q_PROPERTY(QString typedError READ typedError NOTIFY presentationChanged)
    Q_PROPERTY(quint64 passwordRequestId READ passwordRequestId NOTIFY passwordRequestChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY presentationChanged)
    Q_PROPERTY(int currentPage READ currentPage NOTIFY presentationChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY presentationChanged)
    Q_PROPERTY(int rotationDegrees READ rotationDegrees NOTIFY presentationChanged)
    Q_PROPERTY(bool incomplete READ incomplete NOTIFY presentationChanged)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY presentationChanged)
    Q_PROPERTY(bool unsupported READ unsupported NOTIFY presentationChanged)
    Q_PROPERTY(int commandEpoch READ commandEpoch NOTIFY commandEpochChanged)
    /// The active tool id (issue #103). One of the vocabulary names in
    /// docs/INTERACTION_CONTRACT.md; the toolbar binds to it as a single-select.
    Q_PROPERTY(QString activeTool READ activeTool NOTIFY activeToolChanged)
    Q_PROPERTY(QObject* preflight READ preflight CONSTANT)
    Q_PROPERTY(QObject* actionList READ actionList CONSTANT)
    Q_PROPERTY(QObject* inspector READ inspector CONSTANT)
    Q_PROPERTY(QObject* preview READ preview CONSTANT)
    Q_PROPERTY(QObject* documentModel READ documentModel CONSTANT)
    Q_PROPERTY(QObject* focusRestoration READ focusRestoration CONSTANT)
    Q_PROPERTY(QString preflightStateName READ preflightStateName NOTIFY presentationChanged)
    Q_PROPERTY(QString preflightRunUnavailableReason READ preflightRunUnavailableReason NOTIFY presentationChanged)
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

    // ---- Shell settings and diagnostics (#239) -------------------------------------
    // The settings store is the one typed home of the persisted render options;
    // QML renders its rows and toggles them through its typed invokables. The
    // diagnostics bundle projection names the last collection outcome, never a
    // second copy of the bundle's content.
    Q_PROPERTY(QObject* settings READ settings CONSTANT)
    Q_PROPERTY(QString settingsNotice READ settingsNotice NOTIFY presentationChanged)
    Q_PROPERTY(bool settingsResetPending READ settingsResetPending NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap diagnosticsBundle READ diagnosticsBundle NOTIFY presentationChanged)

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
    Q_PROPERTY(QString previewFidelityStateName READ previewFidelityStateName NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap previewFidelityVisual READ previewFidelityVisual NOTIFY presentationChanged)
    Q_PROPERTY(QColor previewFidelityColor READ previewFidelityColor NOTIFY presentationChanged)
    Q_PROPERTY(QString previewFidelityOriginName READ previewFidelityOriginName NOTIFY presentationChanged)
    Q_PROPERTY(QString previewFidelitySummary READ previewFidelitySummary NOTIFY presentationChanged)
    Q_PROPERTY(bool previewRequiresAuthoritative READ previewRequiresAuthoritative NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap compareReview READ compareReview NOTIFY presentationChanged)

    // ---- Display colour modes (#235 display half) -----------------------------------
    // Presentation-only display modes. They filter the pixels the canvas already
    // holds; they ask for no render, they join no surface identity, and the
    // fidelity indicator reports them as a simulation.
    Q_PROPERTY(QString displayColorModeName READ displayColorModeName NOTIFY presentationChanged)
    Q_PROPERTY(bool displayColorModeActive READ displayColorModeActive NOTIFY presentationChanged)
    Q_PROPERTY(QColor displayColorCustomForeground READ displayColorCustomForeground NOTIFY presentationChanged)
    Q_PROPERTY(QColor displayColorCustomBackground READ displayColorCustomBackground NOTIFY presentationChanged)
    Q_PROPERTY(int displayColorBitonalThreshold READ displayColorBitonalThreshold NOTIFY presentationChanged)

    // ---- Soft proof, plates and ink coverage (#237) ---------------------------------
    Q_PROPERTY(bool softProofActive READ softProofActive NOTIFY presentationChanged)
    Q_PROPERTY(QString softProofStateName READ softProofStateName NOTIFY presentationChanged)
    Q_PROPERTY(QString softProofProfileName READ softProofProfileName NOTIFY presentationChanged)
    Q_PROPERTY(QString softProofProfileId READ softProofProfileId NOTIFY presentationChanged)
    Q_PROPERTY(QString softProofProfileTypeName READ softProofProfileTypeName NOTIFY presentationChanged)
    Q_PROPERTY(QString softProofSummary READ softProofSummary NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList productionPlates READ productionPlates NOTIFY presentationChanged)
    Q_PROPERTY(QString plateIsolationSummary READ plateIsolationSummary NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap inkCoverageReadout READ inkCoverageReadout NOTIFY presentationChanged)
    Q_PROPERTY(qreal currentPageCentreX READ currentPageCentreX NOTIFY presentationChanged)
    Q_PROPERTY(qreal currentPageCentreY READ currentPageCentreY NOTIFY presentationChanged)

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

    /// `settings` is the one typed settings store. When null, the host opens
    /// the application's own user-scope store on first use; an injected store
    /// stays owned by the caller and must outlive the host.
    explicit EditorHost(EditorSettings* settings = nullptr, QObject* parent = nullptr);
    ~EditorHost() override;

    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    QString documentState() const;
    bool hasDocument() const;
    bool correctionsAllowed() const;
    QString displayTitle() const;
    QString typedError() const;
    quint64 passwordRequestId() const;
    Q_INVOKABLE bool submitPassword(quint64 requestId, const QString& password);
    Q_INVOKABLE bool cancelPassword(quint64 requestId);
    int pageCount() const;
    int currentPage() const;
    qreal zoom() const;
    int rotationDegrees() const;
    bool incomplete() const;
    bool cancelled() const;
    bool unsupported() const;
    /// The operator's active tool as a vocabulary name (issue #103).
    QString activeTool() const;
    int commandEpoch() const noexcept { return m_commandEpoch; }

    QObject* preflight();
    QObject* actionList();
    QObject* inspector();
    QObject* preview();
    QObject* documentModel() { return &m_documentModel; }
    FocusRestoration* focusRestoration() { return &m_focusRestoration; }

    QString preflightStateName() const;
    QString preflightRunUnavailableReason() const;

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

    /// The one typed settings store (#239). QML renders its option rows and
    /// calls its typed invokables; nothing else owns a settings copy.
    QObject* settings() { return m_settings; }

    /// The settings store's visible notice: a persisted value that was rejected
    /// and replaced, or the last refused settings operation. Empty when nothing
    /// needs reporting.
    QString settingsNotice();

    /// True while a confirmed reset-to-factory is armed and awaiting the
    /// operator's confirmation.
    bool settingsResetPending() const noexcept { return m_settingsResetPending; }

    /// Outcome of the last diagnostics collection: `status` is `idle`, `ready`
    /// or `failed`, with the bundle directory and file count on success and a
    /// typed error on failure.
    QVariantMap diagnosticsBundle() const;

    /// Arms the reset-to-factory confirmation and opens the settings surface.
    /// The reset itself only runs through confirmSettingsReset().
    Q_INVOKABLE bool requestSettingsReset();
    Q_INVOKABLE bool confirmSettingsReset();
    Q_INVOKABLE void cancelSettingsReset();

    /// Collects the ADR-006 diagnostics bundle under the operator-chosen parent
    /// directory and projects the outcome. QML passes a folder URL; paths stay
    /// in C++.
    Q_INVOKABLE bool collectDiagnosticsFileUrl(const QUrl& parentDirectory);

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

    /// Render fidelity and origin of the interactive preview (#28), the same fact on the
    /// ordinary canvas and in the Production Preview. `previewFidelityStateName` is one of
    /// `unavailable`, `stale`, `exact`, `approximate`, `authoritative`, `display-filter` or
    /// `soft-proof`, derived by `pdfquick::tokens::classifyPreviewFidelityState()` from the
    /// preview state, the display simulation in force and the current page's render
    /// diagnostics; `previewFidelityOriginName` names the render path (`none`, `fast-canvas`,
    /// `output-preview`, `display-filter`, `soft-proof`). `previewRequiresAuthoritative` is
    /// true for the overprint-sensitive fast render (`approximate`), whose pixels are not
    /// proof of print-safe output. The interactive preview never certifies publication
    /// safety: only `authoritative` names the output-preview origin, and no state reaches a
    /// pass.
    QString previewFidelityStateName() const;
    QVariantMap previewFidelityVisual() const;
    QColor previewFidelityColor() const;
    QString previewFidelityOriginName() const;
    QString previewFidelitySummary() const;
    bool previewRequiresAuthoritative() const;

    // ---- Display colour modes (#235 display half) -----------------------------------
    /// The display colour mode in force, by vocabulary name, and the settings the
    /// custom mode reads. A display mode is a presentation filter: it changes what
    /// the canvas shows, never what the document is, and it is reported as a
    /// simulation by the fidelity indicator.
    QString displayColorModeName() const;
    bool displayColorModeActive() const;
    QColor displayColorCustomForeground() const;
    QColor displayColorCustomBackground() const;
    int displayColorBitonalThreshold() const;
    /// Parses the mode name once at this boundary. An unknown name is refused and
    /// changes nothing.
    Q_INVOKABLE bool setDisplayColorMode(const QString& modeName);
    Q_INVOKABLE bool setDisplayColorCustomColors(const QColor& foreground, const QColor& background);
    Q_INVOKABLE bool setDisplayColorBitonalThreshold(int threshold);

    // ---- Soft proof (#237) -----------------------------------------------------------
    /// True while the canvas is showing the proofing simulation.
    bool softProofActive() const;
    /// `off`, `simulating`, `profile-missing` or `profile-invalid`. A document
    /// whose output-intent profile is absent or unreadable is a visible state: the
    /// proof is refused with the reason, never silently rendered as sRGB.
    QString softProofStateName() const;
    QString softProofProfileName() const;
    QString softProofProfileId() const;
    QString softProofProfileTypeName() const;
    QString softProofSummary() const;
    /// Enables or disables the proofing simulation. Enabling resolves the
    /// document's output-intent profile through Core's colour management; a
    /// document without a usable one returns false and reports `profile-missing`
    /// or `profile-invalid`.
    Q_INVOKABLE bool setSoftProofEnabled(bool enabled);
    Q_INVOKABLE bool toggleSoftProof();

    // ---- Plates and ink coverage (#237) ----------------------------------------------
    /// The separations Core resolves for the open document: the process plates and
    /// the document's spot plates, each with its visibility and whether it is the
    /// isolated plate. Presentation state: hiding or isolating a plate changes what
    /// the preview shows, never the document.
    QVariantList productionPlates() const;
    QString plateIsolationSummary() const;
    Q_INVOKABLE bool setPlateVisible(const QString& plateId, bool visible);
    /// Shows exactly one plate and hides the others. An unknown plate id is refused.
    Q_INVOKABLE bool isolatePlate(const QString& plateId);
    Q_INVOKABLE bool showAllPlates();
    /// Renders the current page through Core's separation-accurate path and reports
    /// the ink coverage at one page-space point. Asynchronous: the readout lands in
    /// inkCoverageReadout() when the job finishes.
    Q_INVOKABLE bool requestInkCoverageAtPoint(qreal pageX, qreal pageY);
    QVariantMap inkCoverageReadout() const;
    /// The centre of the current page's crop box in page space (PDF user space, the
    /// same space requestInkCoverageAtPoint takes). The host computes it so a
    /// surface can offer a point readout without deriving document geometry.
    qreal currentPageCentreX() const;
    qreal currentPageCentreY() const;

    /// Read-only composition of the Core comparison facts for the Compare workspace:
    /// the before/after artifact identities, the technical finding delta, the attributes
    /// the comparison preserved, and the risk it left unresolved. Every field is a
    /// projection of fixPlanIdentity, fixPreview, fixRecheck and fixSignOff (Core's own
    /// run result), never a second comparison model. `blocked`/`blockedReason` name why a
    /// stale preview or a plan digest that does not match the plan on screen may not be
    /// presented as current.
    QVariantMap compareReview() const;

    /// Moves to the step or finding behind one entry of compareReview()'s
    /// `materialDeltas`. Navigation only: it plans, approves and executes nothing.
    Q_INVOKABLE bool navigateCompareDelta(int deltaIndex);

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
    Q_INVOKABLE void discardActionListPlan();
    Q_INVOKABLE QVariantMap repairParameterSchemaForOperation(const QString& operationId) const;

    // ---- Governed-correction review intents (#586) --------------------------------
    /// The operator's review of the planned correction. Approving binds the decision to
    /// the exact plan digest for the current revision and arms execution; a plan whose
    /// revision or digest moved is never armed. Rejecting records the decision and leaves
    /// the plan visible for inspection; replanning clears both.
    /// QML must supply the displayed identities; the no-argument helper is C++ only.
    bool approveActionListPlan();
    Q_INVOKABLE bool approveActionListPlan(const QString& planDigest,
                                           const QString& sourceSha256,
                                           const QString& documentRevision);
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

    /// The explicit switch the preview surfaces call before a page is presented as proof:
    /// when the current page is the overprint-sensitive fast render, it moves that page to
    /// the authoritative output-preview render and returns true. A page that is already
    /// authoritative, is exact, or has no document returns false and is left alone.
    Q_INVOKABLE bool ensureAuthoritativePreview();

    Q_INVOKABLE void goToPage(int pageIndex);
    Q_INVOKABLE void goToOutlinePage(int pageIndex);
    Q_INVOKABLE void setWorkspace(LoopWorkspace workspace);

    /// Selects a recipe that runs `operationId` and routes to the Fix workspace, so the
    /// Inspect workspace's "plan a fix for this finding" intent lands somewhere that can
    /// actually plan it. Nothing is planned, approved or executed here.
    Q_INVOKABLE bool selectActionListRecipeForOperation(const QString& operationId);

    /// The Inspect workspace's corrective intent: selects and binds, then stops.
    void onCorrectiveOperationRequested(const pdfinteraction::InspectorCorrectiveOperationIntent& intent);
    /// Every registered workspace resolves to a real surface; the public
    /// invokable still rejects an out-of-range value fail-closed, so a
    /// non-QML caller cannot route past the shell's policy.
    Q_INVOKABLE bool isWorkspaceEnabled(LoopWorkspace workspace) const;
    Q_INVOKABLE void acknowledgeWorkspaceRequest();
    Q_INVOKABLE void acknowledgeSearchPanel();

    /// Selects the active tool by vocabulary name (issue #103). Parses once at
    /// this boundary and returns false for a name outside the vocabulary,
    /// leaving the active tool unchanged. The interaction controller cancels an
    /// in-flight drag with ToolChanged (#141 AC3).
    Q_INVOKABLE bool setActiveTool(const QString& toolId);

    // ---- Select, copy and capture (issue #232) -------------------------------------
    /// Copies the canvas text selection to the clipboard. The clipboard is only
    /// written when there is text to copy; a page with no text layer reports
    /// that state and an empty string is never copied as success.
    Q_INVOKABLE bool copySelectedText();

    /// Writes the visible canvas region to `url` as a PNG through the atomic
    /// writer, which refuses to overwrite an existing file.
    Q_INVOKABLE bool captureCanvasFileUrl(const QUrl& url);

    /// Extracts the current page's images into `directory` through the atomic
    /// writer. Refuses up front when any target file already exists, so a run
    /// cannot half-overwrite the destination.
    Q_INVOKABLE bool extractCurrentPageImagesFileUrl(const QUrl& directory);

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
    void passwordRequestChanged();
    /// A completed drag was refused rather than committed (#206). Both values are the
    /// target's stable names so a surface can explain the refusal.
    void dragRefused(const QString& targetKind, const QString& targetId);
    void presentationChanged();
    void commandEpochChanged();
    void activeToolChanged();
    void workspaceChanged(LoopWorkspace from, LoopWorkspace to);
    void preflightProfilesChanged();
    void preflightProfileDraftChanged();
    void actionListRecipesChanged();
    void preflightReportExportRequested();
    void preflightProfileImportRequested();
    void preflightProfileExportRequested();
    void preflightProfileSaveRequested();

    /// The Screenshot command needs a destination, so the shell asks for one
    /// and answers with captureCanvasFileUrl(). The command itself writes
    /// nothing.
    void screenshotRequested();

    /// The Extract Image command needs a destination directory, so the shell
    /// asks for one and answers with extractCurrentPageImagesFileUrl().
    void imageExtractionRequested();

    /// The shell-side surfaces (#239). The commands emit these; QML owns the
    /// dialogs and opens them.
    void settingsPanelRequested();
    void settingsResetRequested();
    void diagnosticsCollectionRequested();

private:
    void connectFacade();
    void connectViewport();
    void connectCatalog();
    void connectInteraction();
    void connectSurfaces();
    void registerShellHandlers();
    void registerFeatureHandlers();
    void refreshFeatureAvailability();
    bool commandPermissionAllowed(const QString& commandId) const;
    bool requestMoveSelection(const QVariantMap& parameters);
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
    void finishRollbackJob(const pdf::PDFJobSnapshot& snapshot);
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
    static QString dragRefusalMessage(pdfinteraction::InteractionTargetKind kind);
    void onDragCompleted(pdfinteraction::DragSession session);
    void onInteractionSelectionChanged(pdfinteraction::InteractionTarget target);
    void syncProductionState();
    void applyInspectorSelection(const pdfinteraction::InteractionTarget& target);
    void applyEmptyCanvasInspectorSelection();
    void setInspectionMode(QString mode);

    /// The visible canvas region as one image, or a null image when no admitted
    /// tile intersects it or the snapshot is stale.
    QImage composeVisibleCanvasImage() const;
    // ---- Display colour modes, soft proof and plates (#235 display half, #237) -------
    void applyDisplayColorToCanvas();
    /// The simulation the fidelity indicator must name, or empty when the display
    /// shows the render path's own pixels: `soft-proof` or `display-filter`.
    QString displaySimulationName() const;
    struct SoftProofProfile
    {
        QString id;
        QString name;
        QString typeName;
    };
    /// Resolves the document's output-intent profile through Core's colour
    /// management. `documentDeclaresOutputIntent` distinguishes "the document
    /// names no output intent" from "the one it names could not be read", which
    /// are two different visible states. Nothing is resolved without a document.
    std::optional<SoftProofProfile> resolveOutputIntentProfile(bool* documentDeclaresOutputIntent) const;
    /// Caches the separations Core resolves for the open document, keyed by
    /// colorant name, keeping the operator's visibility state for plates that
    /// are still present.
    void reloadProductionPlates();
    QVariantMap plateRow(const QString& id) const;
    QPointF currentPageCentre() const;
    void finishInkCoverageJob(const pdf::PDFJobSnapshot& snapshot);
    /// Pushes the typed render options into the shared render presentation
    /// policy and re-requests the surfaces whose keys the change superseded.
    void applyRenderOptions();

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

    /// The settings store: injected by the caller, or owned here when the host
    /// was constructed without one. See the constructor's contract.
    std::unique_ptr<EditorSettings> m_ownedSettings;
    EditorSettings* m_settings = nullptr;
    bool m_settingsResetPending = false;

    /// The last diagnostics collection's outcome. A projection of the
    /// collector's own result, never a second copy of the bundle.
    struct DiagnosticsBundleState
    {
        QString status = QStringLiteral("idle");
        QString directory;
        QString error;
        int fileCount = 0;
    };
    DiagnosticsBundleState m_diagnosticsBundle;

    QPointer<pdfquick::LoopCanvasItem> m_canvas;
    QHash<QString, pdf::PDFJobKind> m_activeAsyncJobs;
    struct PreflightWorkerOutcome;
    QHash<QString, std::shared_ptr<PreflightWorkerOutcome>> m_preflightOutcomes;
    QHash<QString, std::shared_ptr<pdfinteraction::ActionListWorkerOutcome>> m_actionListOutcomes;
    /// Result of one scheduled rollback. The worker thread fills it; the interactive
    /// thread reads it in finishRollbackJob, so it crosses threads only through the
    /// scheduler's jobFinished dispatch.
    struct RollbackJobOutcome
    {
        bool ok = false;
        QString errorMessage;
        QString destinationPath;
        QString revision;   // digest prefix for the announcement
        bool retentionSuccess = true;
        QString retentionError;
    };
    QHash<QString, std::shared_ptr<RollbackJobOutcome>> m_rollbackOutcomes;

    /// Display colour mode in force (issue #235 display half). Presentation state:
    /// pushed to the canvas item, never to a render request.
    pdfquick::CanvasDisplayColorSettings m_displayColor;

    /// Soft proof state (issue #237). The profile is the document's output-intent
    /// profile Core resolved; `m_softProofStateName` is `off`, `simulating`,
    /// `profile-missing` or `profile-invalid`.
    bool m_softProofEnabled = false;
    QString m_softProofStateName = QStringLiteral("off");
    SoftProofProfile m_softProofProfile;

    /// Separations Core resolved for the open document, and the operator's own
    /// presentation state over them. Visibility is keyed by colorant name so a
    /// plate that is still present keeps its state across a reload.
    QVariantList m_plates;
    QHash<QString, bool> m_plateVisible;
    QString m_isolatedPlateId;

    /// The last ink-coverage readout, and the worker outcomes still in flight.
    QVariantMap m_inkCoverageReadout;
    struct InkCoverageOutcome
    {
        bool ok = false;
        QString reason;
        int pageIndex = -1;
        QVariantMap readout;
    };
    QHash<QString, std::shared_ptr<InkCoverageOutcome>> m_inkCoverageOutcomes;
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
