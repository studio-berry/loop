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

#include <QtTest>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPainter>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <memory>

#include "processoutputcapture.h"

#include "documentviewsession.h"
#include "editorhost.h"
#include "editorsettings.h"
#include "loopstatevisual.h"
#include "looptokens.h"
#include "pdfrenderer.h"
#include "quickdocumentmodel.h"

#include "pdfapplicationidentity.h"
#include "pdfauthoringedit.h"
#include "pdfannotation.h"
#include "pdfblockingthreadguard.h"
#include "pdfcatalog.h"
#include "pdfannotation.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfdocumentreader.h"
#include "pdfpage.h"
#include "pdfencoding.h"
#include "pdfform.h"
#include "quickoutput.h"
#include <QImageReader>
#include <QImageWriter>
#include "pdfsecurityhandler.h"
#include "pdfoperationhistorystore.h"
#include "pdfsettings.h"
#include "pdfworkloadenvelope.h"
#include "preflightprofileresolver.h"

#include "hittestsource.h"
#include "inputintent.h"
#include "interactioncontroller.h"
#include "pagesurfacecoordinator.h"
#include "viewportcontroller.h"

namespace
{

/// Writes a `pageCount`-page fixture and returns its path, or an empty string
/// when the write failed.
QString writePagesFixture(const QString& path, int pageCount)
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 612, 792));
    }
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult written = writer.write(path, &document, true);
    return written ? path : QString();
}

/// The terminal result of one invocation, which the authoring handlers emit
/// synchronously.
std::optional<pdfinteraction::CommandResult> terminalResultFor(QSignalSpy& finished, quint64 invocation)
{
    for (int index = 0; index < finished.count(); ++index)
    {
        const auto result = finished.at(index).at(0).value<pdfinteraction::CommandResult>();
        if (result.invocation == invocation)
        {
            return result;
        }
    }
    return std::nullopt;
}

/// Waits until no page-surface render is in flight. Reporting viewport geometry
/// asks for the first page surfaces, and the render path borrows the open
/// document for the duration of a render -- so a test that replaces the
/// document next lets that initial demand settle first instead of racing it.
void waitForSurfacesToSettle(EditorHost& host)
{
    pdfinteraction::PageSurfaceCoordinator* surfaces = host.sessionForTest()->surfaces();
    QTRY_VERIFY_WITH_TIMEOUT(surfaces->counters().inFlight == 0, 15000);
}

const pdf::PDFLinkAnnotation* linkOnPage(const pdf::PDFDocument* document, int pageIndex)
{
    if (!document)
    {
        return nullptr;
    }

    const pdf::PDFPage* page = document->getCatalog()->getPage(pageIndex);
    if (!page)
    {
        return nullptr;
    }

    for (const pdf::PDFObjectReference& reference : page->getAnnotations())
    {
        const pdf::PDFAnnotationPtr annotation = pdf::PDFAnnotation::parse(&document->getStorage(), reference);
        if (const auto* link = dynamic_cast<const pdf::PDFLinkAnnotation*>(annotation.data()))
        {
            return link;
        }
    }
    return nullptr;
}

int destinationPageIndex(const pdf::PDFDocument* document, const pdf::PDFDestination& destination)
{
    if (!destination.getPageReference().isValid())
    {
        return -1;
    }

    const size_t index = document->getCatalog()->getPageIndexFromPageReference(destination.getPageReference());
    return index == pdf::PDFCatalog::INVALID_PAGE_INDEX ? -1 : static_cast<int>(index);
}

bool writeEncryptedFixture(const QString& path, pdf::PDFSecurityHandlerFactory::Algorithm algorithm,
                           const QString& userPassword = QStringLiteral("fixture-user"), quint32 permissions = 0)
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    pdf::PDFSecurityHandlerFactory::SecuritySettings settings;
    settings.algorithm = algorithm;
    settings.permissions = permissions;
    settings.userPassword = userPassword;
    settings.ownerPassword = QStringLiteral("fixture-owner");
    settings.id = builder.build().getIdPart(0);
    const auto security = pdf::PDFSecurityHandlerFactory::createSecurityHandler(settings);
    if (!security)
    {
        return false;
    }
    builder.setSecurityHandler(security);
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    if (!writer.write(path, &document, true))
    {
        return false;
    }
    QFile fixture(path);
    if (!fixture.open(QIODevice::ReadOnly))
    {
        return false;
    }
    qInfo().noquote() << "encrypted-fixture" << QFileInfo(path).fileName()
                      << "sha256=" << QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex();
    return true;
}

/// Minimal scripted source for fencing proof, mirroring tst_interactioncontrollertest.
/// Returns targets the test wrote down so hover transitions are deterministic
/// without a corpus.
class ScriptedHitTestSource final : public pdfinteraction::IHitTestSource
{
public:
    QList<pdfinteraction::InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override
    {
        QList<pdfinteraction::InteractionTarget> hits;
        for (const pdfinteraction::InteractionTarget& target : targets)
        {
            if (target.pageIndex == pageIndex && target.pageBounds.contains(pagePoint))
            {
                hits.push_back(target);
            }
        }
        return hits;
    }

    QList<pdfinteraction::InteractionTarget> targets;
};

pdfinteraction::InteractionTarget makeFindingTarget(const QString& id, const QRectF& bounds, int pageIndex = 0)
{
    pdfinteraction::InteractionTarget target;
    target.kind = pdfinteraction::InteractionTargetKind::Finding;
    target.pageIndex = pageIndex;
    target.id = id;
    target.pageBounds = bounds;
    return target;
}

pdfinteraction::PointerIntent makeMoveIntent(QPoint positionPx, quint64 sequence)
{
    pdfinteraction::PointerIntent intent;
    intent.stamp.sequence = sequence;
    intent.stamp.monotonicNs = qint64(sequence) * 1000000;
    intent.action = pdfinteraction::PointerAction::Move;
    intent.positionPx = positionPx;
    intent.button = Qt::NoButton;
    intent.buttons = Qt::NoButton;
    intent.modifiers = Qt::NoModifier;
    return intent;
}

// --- GUI-to-CLI preflight parity support (issue #195) -----------------------
//
// #195's anti-divergence test compares the two preflight surfaces, so the CLI
// oracle invocation and the report normalisation below are copies of
// UnitTestsPreflightCorpus (tst_preflightcorpus.cpp) rather than a second policy
// of this file's own: one PdfTool runner shape, one set of normalised fields.

QString preflightFixturesDir()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures");
}

QString preflightSourceDir()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR);
}

/// Runs the built `PdfTool preflight` and returns the report from the JSON
/// envelope's data.report. See PreflightCorpusTest::runPreflight().
void runPdfToolPreflight(const QString& pdfPath, const QString& profilePath, QJsonObject& report, int& exitCode)
{
    QProcess process;
    // PdfTool constructs a QGuiApplication; force the offscreen platform so the
    // child process can start on headless CI runners with no X display.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral(PDFTOOL_EXECUTABLE_PATH),
                  { QStringLiteral("preflight"),
                    pdfPath,
                    QStringLiteral("--profile"),
                    profilePath,
                    QStringLiteral("--console-format"),
                    QStringLiteral("json") });
    QByteArray stdOut;
    QByteArray stdErr;
    QVERIFY2(test_support::waitForFinishedAndCapture(process, 30000, stdOut, stdErr),
             qPrintable(QStringLiteral("PdfTool preflight timed out: %1\nstderr: %2").arg(process.errorString(), QString::fromUtf8(stdErr))));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("Invalid report JSON: %1\nstderr: %2").arg(parseError.errorString(), QString::fromUtf8(stdErr))));
    QVERIFY2(document.isObject(), "result JSON must be a top-level object");

    const QJsonObject envelope = document.object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(envelope.value(QStringLiteral("command")).toString(), QStringLiteral("preflight"));
    QCOMPARE(envelope.value(QStringLiteral("exit_code")).toInt(), process.exitCode());
    report = envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QVERIFY2(!report.isEmpty(), "preflight result must contain data.report");
    exitCode = process.exitCode();
}

/// See PreflightCorpusTest::normalizeReport(): strip the fields that legitimately
/// vary between runs, checkouts and surfaces and are not part of the check
/// behavior both surfaces must agree on.
QJsonObject normalizePreflightReport(QJsonObject report)
{
    report.remove(QStringLiteral("engine_version"));
    report.remove(QStringLiteral("pdf"));
    report.remove(QStringLiteral("profile_resolution"));
    report.remove(QStringLiteral("document_revision_digest"));
    report.remove(QStringLiteral("effective_profile_digest"));
    report.remove(QStringLiteral("profile_identity"));
    report.remove(QStringLiteral("coverage_scope"));
    report.remove(QStringLiteral("variable_bindings"));
    report.remove(QStringLiteral("decisions"));
    for (const QString& section : { QStringLiteral("errors"), QStringLiteral("warnings") })
    {
        QJsonArray findings = report.value(section).toArray();
        for (int index = 0; index < findings.size(); ++index)
        {
            QJsonObject finding = findings.at(index).toObject();
            finding.remove(QStringLiteral("id"));
            finding.remove(QStringLiteral("evidence_ids"));
            findings.replace(index, finding);
        }
        report.insert(section, findings);
    }
    return report;
}

void parsePreflightReport(const QByteArray& bytes, const QString& label, QJsonObject& report)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("%1 is not valid JSON: %2").arg(label, parseError.errorString())));
    QVERIFY2(document.isObject(), qPrintable(QStringLiteral("%1 must be a top-level JSON object").arg(label)));
    report = document.object();
}

/// Names the first few differing lines so a divergence is readable in the log.
/// The assertion stays whole-document equality; this only explains a failure.
QString describePreflightReportDifference(const QString& guiText, const QString& cliText)
{
    const QStringList guiLines = guiText.split(QLatin1Char('\n'));
    const QStringList cliLines = cliText.split(QLatin1Char('\n'));
    const int lineCount = qMax(guiLines.size(), cliLines.size());
    QStringList differences;
    for (int line = 0; line < lineCount && differences.size() < 12; ++line)
    {
        const QString guiLine = line < guiLines.size() ? guiLines.at(line) : QStringLiteral("<missing>");
        const QString cliLine = line < cliLines.size() ? cliLines.at(line) : QStringLiteral("<missing>");
        if (guiLine != cliLine)
        {
            differences << QStringLiteral("line %1: GUI %2\nline %1: CLI %3").arg(line + 1).arg(guiLine.trimmed(), cliLine.trimmed());
        }
    }
    return differences.join(QLatin1Char('\n'));
}

/// A page carrying a text field, a checkbox, a combo box and a signature field,
/// the shape the authoring-edit handlers are driven against. The signature field
/// is present so "signature fields are never filled" has something to exclude.
bool writeFormFixtureDocument(const QString& path)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));

    const auto addFormXObject = [&builder](const QRectF& rect)
    {
        const QByteArray content = QByteArrayLiteral(" ");
        pdf::PDFDictionary dictionary;
        dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
        dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Form"));
        dictionary.addEntry(pdf::PDFInplaceOrMemoryString("BBox"),
                            pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                                pdf::PDFObject::createReal(rect.left()), pdf::PDFObject::createReal(rect.top()),
                                pdf::PDFObject::createReal(rect.right()), pdf::PDFObject::createReal(rect.bottom()) })));
        // Streams carry their own /Length; the writer does not add one.
        dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(content.size()));
        return builder.addObject(pdf::PDFObject::createStream(
            std::make_shared<pdf::PDFStream>(std::move(dictionary), QByteArray(content))));
    };

    const auto addField = [&builder, &page, &addFormXObject](const QByteArray& fieldType,
                                                             const QString& name,
                                                             const QRectF& rect,
                                                             const std::function<void(pdf::PDFDictionary&)>& decorate)
    {
        pdf::PDFDictionary field;
        field.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("Annot"));
        field.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Widget"));
        field.addEntry(pdf::PDFInplaceOrMemoryString("FT"), pdf::PDFObject::createName(fieldType));
        field.addEntry(pdf::PDFInplaceOrMemoryString("T"), pdf::PDFObjectFactory::createTextString(name));
        field.addEntry(pdf::PDFInplaceOrMemoryString("Rect"),
                       pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                           pdf::PDFObject::createReal(rect.left()), pdf::PDFObject::createReal(rect.top()),
                           pdf::PDFObject::createReal(rect.right()), pdf::PDFObject::createReal(rect.bottom()) })));
        field.addEntry(pdf::PDFInplaceOrMemoryString("P"), pdf::PDFObject::createReference(page));
        if (decorate)
        {
            decorate(field);
        }
        const pdf::PDFObjectReference reference =
            builder.addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(field))));

        pdf::PDFDictionary pageAnnots;
        pageAnnots.addEntry(pdf::PDFInplaceOrMemoryString("Annots"),
                            pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                                pdf::PDFObject::createReference(reference) })));
        builder.appendTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageAnnots))));
        return reference;
    };

    std::vector<pdf::PDFObjectReference> fields;
    fields.push_back(addField("Tx", QStringLiteral("LoopText"), QRectF(10, 10, 90, 30),
                              [&addFormXObject](pdf::PDFDictionary& field)
                              {
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("V"),
                                                 pdf::PDFObjectFactory::createTextString(QStringLiteral("original")));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("DA"),
                                                 pdf::PDFObject::createString(QByteArrayLiteral("/Helv 0 Tf 0 g")));
                                  pdf::PDFDictionary appearance;
                                  appearance.addEntry(pdf::PDFInplaceOrMemoryString("N"),
                                                      pdf::PDFObject::createReference(addFormXObject(QRectF(10, 10, 90, 30))));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("AP"),
                                                 pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(appearance))));
                              }));
    fields.push_back(addField("Btn", QStringLiteral("LoopCheck"), QRectF(10, 50, 30, 30),
                              [&addFormXObject](pdf::PDFDictionary& field)
                              {
                                  pdf::PDFDictionary normal;
                                  normal.addEntry(pdf::PDFInplaceOrMemoryString("Yes"),
                                                  pdf::PDFObject::createReference(addFormXObject(QRectF(10, 50, 30, 30))));
                                  normal.addEntry(pdf::PDFInplaceOrMemoryString("Off"),
                                                  pdf::PDFObject::createReference(addFormXObject(QRectF(10, 50, 30, 30))));
                                  pdf::PDFDictionary appearance;
                                  appearance.addEntry(pdf::PDFInplaceOrMemoryString("N"),
                                                      pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(normal))));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("AP"),
                                                 pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(appearance))));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("AS"), pdf::PDFObject::createName("Off"));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("V"), pdf::PDFObject::createName("Off"));
                              }));
    fields.push_back(addField("Ch", QStringLiteral("LoopChoice"), QRectF(10, 90, 90, 30),
                              [&addFormXObject](pdf::PDFDictionary& field)
                              {
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("Ff"), pdf::PDFObject::createInteger(1 << 17));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("Opt"),
                                                 pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                                                     pdf::PDFObjectFactory::createTextString(QStringLiteral("Alpha")),
                                                     pdf::PDFObjectFactory::createTextString(QStringLiteral("Beta")) })));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("V"),
                                                 pdf::PDFObjectFactory::createTextString(QStringLiteral("Alpha")));
                                  pdf::PDFDictionary appearance;
                                  appearance.addEntry(pdf::PDFInplaceOrMemoryString("N"),
                                                      pdf::PDFObject::createReference(addFormXObject(QRectF(10, 90, 90, 30))));
                                  field.addEntry(pdf::PDFInplaceOrMemoryString("AP"),
                                                 pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(appearance))));
                              }));

    const pdf::PDFObjectReference signature = builder.createSignatureDictionary(
        QByteArrayLiteral("Adobe.PPKLite"),
        QByteArrayLiteral("adbe.pkcs7.detached"),
        QByteArrayLiteral("signed-placeholder"),
        QDateTime::currentDateTimeUtc(),
        0);
    const pdf::PDFObjectReference signatureField = builder.createFormFieldSignature(QStringLiteral("LoopSignature"), {}, signature);
    builder.createInvisibleFormFieldWidget(signatureField, page);
    fields.push_back(signatureField);

    builder.createAcroForm(fields);
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    return static_cast<bool>(writer.write(path, &document, true));
}

/// An AcroForm whose only content is an XFA packet: no AcroForm fields at all,
/// which is the form the shell must report as unsupported.
bool writeXfaOnlyFixtureDocument(const QString& path)
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));

    const QByteArray xfaPacket = QByteArrayLiteral("<xdp/>");
    pdf::PDFDictionary xfaDictionary;
    xfaDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(xfaPacket.size()));
    const pdf::PDFObjectReference xfa = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(xfaDictionary), QByteArray(xfaPacket))));

    pdf::PDFDictionary form;
    form.addEntry(pdf::PDFInplaceOrMemoryString("Fields"), pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>()));
    form.addEntry(pdf::PDFInplaceOrMemoryString("XFA"), pdf::PDFObject::createReference(xfa));

    pdf::PDFDictionary catalogForm;
    catalogForm.addEntry(pdf::PDFInplaceOrMemoryString("AcroForm"),
                         pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(form))));
    builder.mergeTo(builder.getCatalogReference(),
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(catalogForm))));

    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    return static_cast<bool>(writer.write(path, &document, true));
}

/// Reads one field's value from an open document, the way an operator would see
/// it. `isName` reports a name value (a button's state) rather than a string.
QString readFormFieldValue(const pdf::PDFDocument* document, const QString& name, bool* isName = nullptr)
{
    if (isName)
    {
        *isName = false;
    }
    if (!document)
    {
        return QString();
    }

    pdf::PDFFormManager manager(nullptr);
    manager.setDocument(pdf::PDFModifiedDocument(const_cast<pdf::PDFDocument*>(document), nullptr, pdf::PDFModifiedDocument::Reset));
    QString value;
    manager.apply([&value, &name, isName](const pdf::PDFFormField* field)
                  {
                      if (!value.isEmpty() || field->getName(pdf::PDFFormField::NameType::FullyQualified) != name)
                      {
                          return;
                      }
                      const pdf::PDFObject& object = field->getValue();
                      if (object.isName())
                      {
                          if (isName)
                          {
                              *isName = true;
                          }
                          value = QString::fromLatin1(object.getString());
                      }
                      else if (object.isString())
                      {
                          value = pdf::PDFEncoding::convertTextString(object.getString());
                      } });
    return value;
}

}   // namespace

class EditorHostTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_fixtureRecipes;

private slots:
    void outputBytesAndFences();
    void outputPublishedPacket();
    void outputRenderFormats_data();
    void outputRenderFormats();
    void outputOverprintWarning();
    void outputRenderFailure();
    void init();
    void cleanup();
    void encryptedOpen_data();
    void encryptedOpen();
    void passwordFailureIsTypedAndRetryable();
    void passwordCancelCloseAndSupersessionAreTerminal();
    void teardownWhilePasswordPromptIsPending();
    void restrictedDocumentCannotPlanOrPublish();
    void encryptedInspectionReceiptContainsNoPassword();
    void teardownClearsTheInteractiveThreadRegistration();
    void startsWithNoDocument();
    void exposesCatalogDescriptorsWithoutMutating();
    void navigationCommandsStayDisabledUntilOpen();
    void sessionTeardownDrainsWorkersBeforeAdapters();
    void preflightRunsOffInteractiveThread();
    void preflightStateVisualIsNotCheckedBeforeARun();
    void preflightFencesCompletionsThatLostTheirRequestIdentity();
    void actionListFencesCompletionsThatLostTheirRequestIdentity();
    void exportedPreflightReportMatchesPdfToolForTheSameInputs();
    void restrictedPreflightReportMatchesPdfTool();
    void importValidProfileAddsDigest();
    void importDigestMismatchProfileIsRejected();
    void saveProfileForkRecordsDerivedFrom();
    void openLargeDocument();

    // Workspace surfaces (#586).
    void fixWorkspacePresentsIdleLifecycleAndRefusesToArm();
    void fixReviewBindsToThePlannedDigestAndTheCurrentRevision();
    void moveSelectionProposesAPageBoxMoveInTheFixWorkspace();
    void completedDragOfARefusedKindIsReportedAndChangesNothing();
    void shippedRecipeIsListedButNeverSelectedByDefault();
    void shippedPageBoxRecipeRunsDefaultMoveThroughPlanApprovalAndRevalidation();
    void operatorRecipeWinsOverShippedRecipe();
    void invalidOperatorRecipeStopsMoveWithoutShippedFallback();
    void fixJourneyPublishesOnlyAnApprovedPlanBoundToTheDisplayedIdentity();
    void translatePageBoxRefusesAMoveThatIntroducesABlockingFinding();
    void executeApprovedActionListPlanRefusesAnUnreviewedPlan();
    void fixRollbackReturnsToARecordedRevision();
    void annotationReviewListsAndNavigatesToThePageRegion();
    /// Issue #234: annotation authoring commands go through the direct-edit
    /// gateway, and the authored annotation reaches the quick lane's models.
    void annotationCommandsAuthorThroughTheGatewayAndRefuseWithoutGeometry();

    void signatureStatusPresentsTheReducedState();
    void signatureInvalidationGateBlocksExecutionUntilAcknowledged();
    void previewFidelityNamesTheOriginAndSwitchesExplicitly();
    void refusedRenderCannotBecomeAuthoritativeEvidence();
    void displayColorModesChangePresentationAndAreNeverProductionEvidence();
    void displayColorModeNamesAreParsedOnceAtTheBoundary();
    void softProofSimulatesThroughCoreColourManagementAndNeverBecomesEvidence();
    void softProofWithoutAUsableOutputIntentIsAVisibleState();
    void platesListSeparationsAndCarryVisibilityWithoutTouchingTheDocument();
    void inkCoverageReadoutAtAPointIsMeasuredOrExplicitlyUnavailable();

    // Shell settings, render options and diagnostics (#239).
    void settingsRoundTripThroughTheOneStore();
    void settingsFactoryResetIsConfirmedAndComplete();
    void invalidPersistedRenderOptionIsReportedAndReplaced();
    void renderOptionsReachTheRendererAndTheFidelityIndicator();
    void collectDiagnosticsProducesTheAdrBundle();

    /// Issue #232: extraction refuses to overwrite an existing file up front.
    void imageExtractionRefusesToOverwriteAnExistingFile();

    // Bookmarks and hyperlinks (#233, ADR-014).
    void bookmarkCommandsWriteTheOutlineThroughTheCatalog();
    void bookmarkImportExportRoundTripsAndRefusesAStaleRevision();
    void hyperlinkCommandsWriteEveryDestinationVariant_data();
    void hyperlinkCommandsWriteEveryDestinationVariant();
    void restrictedDocumentCannotEditBookmarksOrLinks();

    // Direct authoring edits (ADR-014, issue #242): the catalog route into Core,
    // the revision fence, the XFA refusal, the attachment path and the
    // send-by-email hand-off.
    void formValueEditRoutesThroughTheCatalogIntoCoreAndProducesANewRevision();
    void authoringEditRefusesAStaleRevisionAndChangesNothing();
    void xfaOnlyFormIsReportedAsUnsupportedThroughTheShell();
    void attachmentEditsAndExtractionRouteThroughTheShell();
    void sendByEMailHandsOverOnlyThePublishedRevision();
};

void EditorHostTest::init()
{
    QStandardPaths::setTestModeEnabled(true);
    qRegisterMetaType<pdfinteraction::CommandResult>("pdfinteraction::CommandResult");
    m_fixtureRecipes = std::make_unique<QTemporaryDir>();
    QVERIFY(m_fixtureRecipes->isValid());
}

void EditorHostTest::cleanup()
{
    m_fixtureRecipes.reset();
}

void EditorHostTest::encryptedOpen_data()
{
    QTest::addColumn<int>("algorithm");
    QTest::addColumn<bool>("owner");
    QTest::addColumn<quint32>("userPermissions");
    QTest::newRow("AES-128-user") << int(pdf::PDFSecurityHandlerFactory::AES_128) << false << quint32(0);
    QTest::newRow("AES-128-owner") << int(pdf::PDFSecurityHandlerFactory::AES_128) << true << quint32(0);
    QTest::newRow("AES-256-user") << int(pdf::PDFSecurityHandlerFactory::AES_256) << false << quint32(0);
    QTest::newRow("AES-256-owner") << int(pdf::PDFSecurityHandlerFactory::AES_256) << true << quint32(0);
    using Permission = pdf::PDFSecurityHandler::Permission;
    const quint32 editing = quint32(Permission::Modify) | quint32(Permission::Assemble);
    QTest::newRow("AES-256-edit-allowed-copy-print-restricted") << int(pdf::PDFSecurityHandlerFactory::AES_256) << false << editing;
    const quint32 all = editing | quint32(Permission::CopyContent) | quint32(Permission::PrintLowResolution);
    QTest::newRow("AES-256-unrestricted-user") << int(pdf::PDFSecurityHandlerFactory::AES_256) << false << all;
}

void EditorHostTest::encryptedOpen()
{
    QFETCH(int, algorithm);
    QFETCH(bool, owner);
    QFETCH(quint32, userPermissions);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("encrypted.pdf"));
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::Algorithm(algorithm), QStringLiteral("fixture-user"), userPermissions));
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    const quint64 request = host.passwordRequestId();
    QVERIFY(!host.submitPassword(request + 1, QStringLiteral("fixture-owner")));
    QVERIFY(host.submitPassword(request, owner ? QStringLiteral("fixture-owner") : QStringLiteral("fixture-user")));
    QVERIFY(!host.submitPassword(request, QStringLiteral("fixture-owner")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
    QCOMPARE(host.pageCount(), 1);
    QCOMPARE(host.passwordRequestId(), quint64(0));
    const auto permissions = host.sessionForTest()->facade().permissions();
    QVERIFY(permissions.encrypted);
    using Permission = pdf::PDFSecurityHandler::Permission;
    const bool print = owner || (userPermissions & quint32(Permission::PrintLowResolution));
    const bool modify = owner || (userPermissions & quint32(Permission::Modify));
    const bool copy = owner || (userPermissions & quint32(Permission::CopyContent));
    const bool assemble = owner || (userPermissions & quint32(Permission::Assemble));
    QCOMPARE(permissions.print, print);
    QCOMPARE(permissions.modify, modify);
    QCOMPARE(permissions.copy, copy);
    QCOMPARE(permissions.assemble, assemble);
    QCOMPARE(host.correctionsAllowed(), print && modify && copy && assemble);
    auto* model = qobject_cast<QuickDocumentModel*>(host.documentModel());
    QVERIFY(model);
    QVERIFY(model->encrypted());
    QCOMPARE(model->canPrint(), print);
    QCOMPARE(model->canModify(), modify);
    QCOMPARE(model->canCopy(), copy);
    QCOMPARE(model->canAssemble(), assemble);
    QCOMPARE(host.isCommandEnabled(QStringLiteral("actionSave_As")), host.correctionsAllowed());
}

void EditorHostTest::passwordFailureIsTypedAndRetryable()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("wrong-password-AES-256.pdf"));
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::AES_256));
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    QVERIFY(host.submitPassword(host.passwordRequestId(), QStringLiteral("incorrect-fixture-password")));
    QTRY_COMPARE_WITH_TIMEOUT(host.documentState(), QStringLiteral("error"), 10000);
    QCOMPARE(host.typedError(), QStringLiteral("document/password-incorrect"));
    QCOMPARE(host.sessionForTest()->facade().operation().result.state, pdfinteraction::CommandTerminalState::Failed);
    QVERIFY(!host.hasDocument());
    QVERIFY(host.sessionForTest()->facade().retry() != 0);
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    QVERIFY(host.submitPassword(host.passwordRequestId(), QStringLiteral("fixture-owner")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
}

void EditorHostTest::passwordCancelCloseAndSupersessionAreTerminal()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cancelled-AES-256.pdf"));
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::AES_256));
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    QVERIFY(host.cancelPassword(host.passwordRequestId()));
    QTRY_COMPARE_WITH_TIMEOUT(host.documentState(), QStringLiteral("empty"), 10000);
    QCOMPARE(host.typedError(), QStringLiteral("document/password-cancelled"));
    QVERIFY(host.cancelled());
    QVERIFY(!host.hasDocument());

    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    const quint64 closedRequest = host.passwordRequestId();
    host.invokeCommand(QStringLiteral("actionClose"));
    QVERIFY(!host.submitPassword(closedRequest, QStringLiteral("fixture-owner")));
    QTRY_COMPARE_WITH_TIMEOUT(host.passwordRequestId(), quint64(0), 10000);
    QVERIFY(!host.submitPassword(closedRequest, QStringLiteral("fixture-owner")));
    QVERIFY(!host.hasDocument());

    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    const quint64 supersededRequest = host.passwordRequestId();
    host.openFileUrl(QUrl::fromLocalFile(path));
    QVERIFY(!host.submitPassword(supersededRequest, QStringLiteral("fixture-owner")));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0 && host.passwordRequestId() != supersededRequest, 10000);
    QVERIFY(!host.submitPassword(supersededRequest, QStringLiteral("fixture-owner")));
    QVERIFY(host.submitPassword(host.passwordRequestId(), QStringLiteral("fixture-user")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
}

void EditorHostTest::teardownWhilePasswordPromptIsPending()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("teardown-AES-256.pdf"));
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::AES_256));
    auto host = std::make_unique<EditorHost>(nullptr, nullptr, m_fixtureRecipes->path());
    host->openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host->passwordRequestId() != 0, 10000);
    QElapsedTimer timer;
    timer.start();
    host.reset();
    QVERIFY(timer.elapsed() < 5000);
}

void EditorHostTest::restrictedDocumentCannotPlanOrPublish()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("owner-only-AES-256.pdf"));
    const QString output = directory.filePath(QStringLiteral("refused.pdf"));
    using Permission = pdf::PDFSecurityHandler::Permission;
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::AES_256, {},
                                  quint32(Permission::Modify) | quint32(Permission::Assemble)));
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray original = source.readAll();
    source.close();
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
    QCOMPARE(host.passwordRequestId(), quint64(0));
    QVERIFY(!host.validateActionListRecipe());
    QVERIFY(!host.planActionList());
    QVERIFY(!host.approveActionListPlan());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.fixRollbackAvailable());
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionPrint")));
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionCopyText")));
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionSave_As")));
    host.saveAsFileUrl(QUrl::fromLocalFile(output));
    QVERIFY(!QFileInfo::exists(output));

    auto cancellation = std::make_shared<pdf::PDFJobCancellationToken>();
    pdf::PDFJobContext context(cancellation, pdf::PDFProcessingLimits::conservativeDefaults(), {});
    pdfinteraction::PDFDocumentFileWriter writer;
    const auto writeResult = writer.write({ output }, host.sessionForTest()->context().getDocument(), context);
    QCOMPARE(writeResult.outcome, pdfinteraction::DocumentWriteOutcome::Failed);
    QCOMPARE(writeResult.typedError, QStringLiteral("document/correction-permission-denied"));
    pdf::PDFActionList recipe;
    recipe.id = QStringLiteral("restricted-bleed");
    pdf::PDFActionListStep step;
    step.id = QStringLiteral("bleed");
    step.operationId = QStringLiteral("add-bleed");
    step.parameters = { { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } };
    recipe.steps.append(step);
    for (const auto phase : { pdfinteraction::ActionListRunPhase::Plan, pdfinteraction::ActionListRunPhase::Execute })
    {
        const auto outcome = std::make_shared<pdfinteraction::ActionListWorkerOutcome>();
        const auto worker = pdfinteraction::makeActionListRunWorker(phase, recipe,
                                                                    host.sessionForTest()->context().getDocumentPointer(), {}, outcome);
        worker(context);
        QVERIFY(!outcome->ok);
        QVERIFY(!outcome->candidate);
        QCOMPARE(outcome->executionResult.status, QStringLiteral("failed"));
        QCOMPARE(outcome->executionResult.diagnostics.first().toObject().value(QStringLiteral("code")).toString(),
                 QStringLiteral("document/correction-permission-denied"));
    }
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(source.readAll() == original);
}

void EditorHostTest::encryptedInspectionReceiptContainsNoPassword()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("inspection-AES-256.pdf"));
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::AES_256));
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    QVERIFY(host.submitPassword(host.passwordRequestId(), QStringLiteral("fixture-user")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 30000);
    QVERIFY2(host.hasPreflightReport(), qPrintable(host.preflightOperatorSummary()));
    pdf::PDFOperationHistoryStore history(path + QStringLiteral(".loop-history/history.sqlite3"));
    QString error;
    QVERIFY(history.open(&error));
    const auto events = history.events(&error);
    QVERIFY(error.isEmpty());
    bool receiptFound = false;
    for (const auto& event : events)
    {
        const auto object = event.resultSummary.value(QStringLiteral("inspection_receipt")).toObject();
        if (object.isEmpty())
        {
            continue;
        }
        pdf::PreflightInspectionReceipt receipt;
        QVERIFY2(pdf::preflightInspectionReceiptFromJson(object, receipt, error), qPrintable(error));
        QVERIFY(receipt.limitations.contains(QStringLiteral("Document is encrypted; authenticated document permissions apply.")));
        const QByteArray json = QJsonDocument(object).toJson();
        QVERIFY(!json.contains("fixture-user"));
        QVERIFY(!json.contains("fixture-owner"));
        receiptFound = true;
    }
    QVERIFY(receiptFound);
}

void EditorHostTest::teardownClearsTheInteractiveThreadRegistration()
{
    QVERIFY(!pdf::PDFBlockingThreadGuard::isInteractiveThreadRegistered());
    {
        EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
        QVERIFY(pdf::PDFBlockingThreadGuard::isInteractiveThreadRegistered());
        QVERIFY(pdf::PDFBlockingThreadGuard::isCurrentThreadInteractive());
    }
    // The host registered its owning thread; it must take the registration with
    // it, or a host recreated in the same process leaves a stale one behind that
    // keeps refusing synchronous blocking work.
    QVERIFY(!pdf::PDFBlockingThreadGuard::isInteractiveThreadRegistered());
}

void EditorHostTest::startsWithNoDocument()
{
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QCOMPARE(host.documentState(), QStringLiteral("empty"));
    QVERIFY(!host.hasDocument());
    QCOMPARE(host.pageCount(), 0);

    const QVariantList profiles = host.preflightProfiles();
    QVERIFY2(!profiles.isEmpty(), "the GUI must expose at least one bundled preflight profile");
    const QVariantMap defaultProfile = profiles.constFirst().toMap();
    QVERIFY(defaultProfile.value(QStringLiteral("valid")).toBool());
    QVERIFY(!defaultProfile.value(QStringLiteral("digest")).toString().isEmpty());
    QVERIFY(defaultProfile.value(QStringLiteral("diagnostic")).toString().isEmpty());
    QVERIFY(host.actionList());
    QCOMPARE(host.actionListStateName(), QStringLiteral("idle"));
    QVERIFY(!host.repairOperations().isEmpty());
    QVERIFY(!host.planActionList());
}

void EditorHostTest::exposesCatalogDescriptorsWithoutMutating()
{
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    const QVariantList descriptors = host.commandDescriptors();
    QVERIFY(descriptors.size() > 100);

    bool sawOpen = false;
    for (const QVariant& entryVariant : descriptors)
    {
        const QVariantMap entry = entryVariant.toMap();
        if (entry.value(QStringLiteral("id")).toString() == QStringLiteral("actionOpen"))
        {
            sawOpen = true;
            QVERIFY(entry.value(QStringLiteral("implemented")).toBool());
            QVERIFY(!host.shortcutForCommand(QStringLiteral("actionOpen")).isEmpty() || entry.contains(QStringLiteral("shortcut")));
        }
    }
    QVERIFY(sawOpen);
}

void EditorHostTest::navigationCommandsStayDisabledUntilOpen()
{
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionGoToNextPage")));
    QCOMPARE(host.invokeCommand(QStringLiteral("actionGoToNextPage")), quint64(0));
}

void EditorHostTest::sessionTeardownDrainsWorkersBeforeAdapters()
{
    auto session = std::make_unique<DocumentViewSession>();
    DocumentViewSession* rawSession = session.get();
    std::atomic_bool started = false;
    std::atomic_bool adapterReached = false;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Background;
    rawSession->scheduler().submit(spec,
                                   [rawSession, &started, &adapterReached](pdf::PDFJobContext& context)
                                   {
                                       started.store(true, std::memory_order_release);
                                       while (!context.isCancellationRequested())
                                       {
                                           QThread::yieldCurrentThread();
                                       }

                                       // The session destructor must join this
                                       // work before destroying the renderer.
                                       rawSession->renderer().shedPrefetchAndQuality();
                                       adapterReached.store(true, std::memory_order_release);
                                   });

    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    session.reset();
    QVERIFY(adapterReached.load(std::memory_order_acquire));
}

void EditorHostTest::preflightRunsOffInteractiveThread()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preflight.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.runPreflight());
    QCOMPARE(host.preflightStateName(), QStringLiteral("running"));
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 30000);
    QVERIFY(host.preflightStateName() != QStringLiteral("error"));
    QCOMPARE(host.preflight()->property("progress").toInt(), 100);

    const QString historyPath =
        QDir(QFileInfo(path).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                        { return event.kind == pdf::PDFOperationHistoryEventKind::DocumentOpened; }));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                        { return event.kind == pdf::PDFOperationHistoryEventKind::PreflightRun &&
                                 event.status == pdf::PDFOperationHistoryStatus::Accepted; }));
    QVERIFY(history.verify().verified);
}

void EditorHostTest::preflightStateVisualIsNotCheckedBeforeARun()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preflight-visual.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // Before any run the badge must say "not checked" and must not be a pass (#195 acceptance 1 and 7).
    const QVariantMap before = host.preflightStateVisual();
    QCOMPARE(before.value(QStringLiteral("kind")).toString(), QStringLiteral("NotChecked"));
    QVERIFY2(before.contains(QStringLiteral("kind")) && before.contains(QStringLiteral("colorRole")) && before.contains(QStringLiteral("icon")) && before.contains(QStringLiteral("accessibleName")),
             "the visual must carry the full canonical treatment for QML to render");
    QCOMPARE(before.value(QStringLiteral("accessibleName")).toString(), QStringLiteral("Not checked"));
    QCOMPARE(host.preflightCertificateStateName(), QStringLiteral("not-certified"));
    QCOMPARE(host.preflightCertificateStateVisual().value(QStringLiteral("kind")).toString(), QStringLiteral("NotChecked"));

    // The colour is resolved from the visual's own colour role, never chosen by the QML child. It must
    // be a real colour and it must not be the pass colour.
    const QColor stateColor = host.preflightStateColor();
    QVERIFY2(stateColor.isValid(), "the state colour must be a resolved QColor, not an unset one");

    const pdfquick::tokens::LoopTheme theme =
        host.highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    QCOMPARE(stateColor, pdfquick::tokens::color(pdfquick::tokens::ColorRole::StateNotChecked, theme));
    QVERIFY(stateColor != pdfquick::tokens::color(pdfquick::tokens::ColorRole::Success, theme));

    // A completed run keeps the same shape, but the values must agree with the state Core actually
    // reached. This half used to assert only key presence and a non-empty `kind`, which every
    // reachable state satisfies - a regression that produced the wrong post-run visual still passed.
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);

    const QString stateName = host.preflightStateName();
    const QVariantMap after = host.preflightStateVisual();
    QVERIFY2(after.contains(QStringLiteral("kind")) && after.contains(QStringLiteral("colorRole")) && after.contains(QStringLiteral("icon")) && after.contains(QStringLiteral("accessibleName")),
             "the visual must carry the full canonical treatment for QML to render");

    const pdfquick::tokens::LoopStateVisual expected = pdfquick::tokens::resolvePreflightStateVisual(stateName);

    // The visual is Core's own state rendered, never re-derived: only a real pass may look like one.
    const bool coreSaysPass = stateName == QStringLiteral("pass");
    QCOMPARE(after.value(QStringLiteral("kind")).toString() == QStringLiteral("Passed"), coreSaysPass);
    if (!coreSaysPass)
    {
        QVERIFY(after.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));
        QVERIFY(after.value(QStringLiteral("icon")).toString() != QStringLiteral("Checkmark"));
        QVERIFY(after.value(QStringLiteral("accessibleName")).toString() != QStringLiteral("Passed"));
    }

    // Every field is the one the same map chose for the state name we just read...
    QCOMPARE(after.value(QStringLiteral("kind")).toString(), pdfquick::tokens::stateKindName(expected.kind));
    QCOMPARE(after.value(QStringLiteral("colorRole")).toString(), pdfquick::tokens::colorRoleName(expected.colorRole));
    QCOMPARE(after.value(QStringLiteral("icon")).toString(), pdfquick::tokens::stateIconName(expected.icon));
    QCOMPARE(after.value(QStringLiteral("accessibleName")).toString(), pdfquick::tokens::stateAccessibleName(expected.kind));

    // ...and the resolved colour is color(role, theme) for that same role, exactly as the pre-run
    // half asserts above.
    QCOMPARE(host.preflightStateColor(), pdfquick::tokens::color(expected.colorRole, theme));
}

void EditorHostTest::exportedPreflightReportMatchesPdfToolForTheSameInputs()
{
    // #195 test strategy: "CLI/GUI parity ... assert the exported JSON is byte-identical to PdfTool
    // preflight output. This is the core anti-divergence test." UnitTestsPreflightCorpus pins the CLI
    // against the committed snapshots; this pins the GUI against the CLI, so the report the operator
    // exports cannot drift from what the tool reports for the same document and profile.
    //
    // color-rgb is the fixture of choice because it is one of the loop-default.json cases that fails
    // with findings, so the comparison covers populated errors and warnings instead of two empty
    // arrays, and loop-default.json is the one profile both surfaces can be given: the Editor ships it
    // as the bundled :/profiles/loop-default.json (LoopEditor/app.qrc aliases exactly the
    // loop-preflight/profiles/loop-default.json the CLI reads from disk).
    const QString fixtureId = QStringLiteral("color-rgb");
    const QString documentPath = QDir(preflightFixturesDir()).filePath(fixtureId + QStringLiteral(".pdf"));
    const QString profilePath = QDir(preflightSourceDir()).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString bundledProfileId = QStringLiteral(":/profiles/loop-default.json");

    // The fixture is committed (see `git ls-files loop-preflight/testdata/fixtures`), so its absence
    // can only mean a broken or sparse checkout - exactly when an anti-divergence guard must fail
    // rather than skip. A QSKIP here let this row pass having compared nothing: QtTest's exit code
    // counts only failing rows, so the slot skipped, the process exited 0 and ctest reported the
    // suite as passed.
    QVERIFY2(QFile::exists(documentPath) && QFile::exists(profilePath),
             qPrintable(QStringLiteral("corpus fixture or profile missing; a deployed checkout must carry both, "
                                       "so this is a broken/sparse checkout rather than a reason to skip the parity guard. "
                                       "Regenerate with LoopGenerateFixtures and commit the output (see "
                                       "loop-preflight/README.md, 'Golden corpus & CI'). missing: document='%1' profile='%2'")
                            .arg(documentPath, profilePath)));

    // "The same profile" has to mean the same bytes, not the same name: the GUI can only select the
    // BUNDLED profile while the CLI is handed the file on disk.
    QFile bundledProfile(bundledProfileId);
    QVERIFY2(bundledProfile.open(QIODevice::ReadOnly), "the bundled loop-default profile is missing from the Editor resources");
    QFile diskProfile(profilePath);
    QVERIFY(diskProfile.open(QIODevice::ReadOnly));
    QCOMPARE(bundledProfile.readAll(), diskProfile.readAll());

    // 1. The CLI's report, from the built PdfTool.
    QJsonObject cliReport;
    int cliExitCode = -1;
    runPdfToolPreflight(documentPath, profilePath, cliReport, cliExitCode);
    QVERIFY(!cliReport.isEmpty());

    // 2. The GUI's report, through the export path the operator uses:
    //    EditorHost::exportPreflightReportFileUrl -> PreflightController::serializedReport.
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    if (host.selectedPreflightProfileId() != bundledProfileId)
    {
        QVERIFY2(host.selectPreflightProfile(bundledProfileId),
                 qPrintable(QStringLiteral("the bundled loop-default profile must be selectable; selected '%1'")
                                .arg(host.selectedPreflightProfileId())));
    }
    QCOMPARE(host.selectedPreflightProfileId(), bundledProfileId);

    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY2(host.hasPreflightReport(),
             qPrintable(QStringLiteral("no report to export: state=%1 summary=%2").arg(host.preflightStateName(), host.preflightOperatorSummary())));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString guiReportPath = directory.filePath(QStringLiteral("gui-report.json"));
    QVERIFY2(host.exportPreflightReportFileUrl(QUrl::fromLocalFile(guiReportPath)),
             qPrintable(QStringLiteral("exporting the report failed: state=%1 summary=%2")
                            .arg(host.preflightStateName(), host.preflightOperatorSummary())));

    QFile guiFile(guiReportPath);
    QVERIFY(guiFile.open(QIODevice::ReadOnly));
    const QByteArray guiReport = guiFile.readAll();
    QVERIFY2(!guiReport.isEmpty(), "the exported report is empty");

    QJsonObject guiReportObject;
    parsePreflightReport(guiReport, QStringLiteral("the exported GUI report"), guiReportObject);

    // Both sides go through the corpus test's own normalisation and are then compared as whole
    // documents, so a difference anywhere - a check status, a finding field, a section the other
    // surface does not write - fails this row.
    const QByteArray guiNormalized = QJsonDocument(normalizePreflightReport(guiReportObject)).toJson(QJsonDocument::Indented);
    const QByteArray cliNormalized = QJsonDocument(normalizePreflightReport(cliReport)).toJson(QJsonDocument::Indented);

    // Normalise EOLs so Windows checkouts (eol=crlf) match QJsonDocument's LF output, as the corpus
    // test does for its snapshots.
    const auto normalizeNewlines = [](QByteArray data)
    {
        data.replace("\r\n", "\n");
        data.replace('\r', '\n');
        return data;
    };

    const QString guiText = QString::fromUtf8(normalizeNewlines(guiNormalized));
    const QString cliText = QString::fromUtf8(normalizeNewlines(cliNormalized));

    qInfo("preflight parity: fixture=%s profile=%s cli_exit_code=%d", qPrintable(fixtureId), qPrintable(profilePath), cliExitCode);
    if (guiText != cliText)
    {
        qWarning().noquote() << "GUI preflight report diverges from PdfTool preflight:\n"
                             << describePreflightReportDifference(guiText, cliText);
    }

    QCOMPARE(guiText, cliText);
}

void EditorHostTest::restrictedPreflightReportMatchesPdfTool()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    pdf::PDFSettings::setSettingsPath(temp.path());
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);
    const QString documentPath = QDir(preflightFixturesDir()).filePath(QStringLiteral("image-dpi-low.pdf"));
    QVERIFY(QFile::exists(documentPath));
    const QJsonObject profile = pdf::exportPreflightProfile(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("loop-test-restricted-parity") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Restricted parity") },
        { QStringLiteral("restrictions"), QJsonObject{
                                              { QStringLiteral("pages"), QStringLiteral("1") },
                                              { QStringLiteral("object_classes"), QJsonArray{ QStringLiteral("image") } } } },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("image-resolution") }, { QStringLiteral("min_dpi"), 300 } } } } });
    const QString profilePath = temp.filePath(QStringLiteral("restricted-parity.json"));
    QFile source(profilePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(profile).toJson();
    QCOMPARE(source.write(bytes), bytes.size());
    source.close();

    QJsonObject cliReport;
    int cliExit = -1;
    runPdfToolPreflight(documentPath, profilePath, cliReport, cliExit);
    QVERIFY(!cliReport.isEmpty());
    const QJsonObject cliScope = cliReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject();
    QCOMPARE(cliScope.value(QStringLiteral("pages")).toArray(), QJsonArray{ 1 });
    QCOMPARE(cliScope.value(QStringLiteral("object_classes")).toArray(), QJsonArray{ QStringLiteral("image") });

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importPreflightProfileFileUrl(QUrl::fromLocalFile(profilePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY(host.hasPreflightReport());

    const QString exportedPath = temp.filePath(QStringLiteral("restricted-gui-report.json"));
    QVERIFY(host.exportPreflightReportFileUrl(QUrl::fromLocalFile(exportedPath)));
    QFile exported(exportedPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QJsonObject guiReport;
    parsePreflightReport(exported.readAll(), QStringLiteral("restricted GUI report"), guiReport);
    QVERIFY(!guiReport.isEmpty());
    QCOMPARE(guiReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject(), cliScope);
    QCOMPARE(QJsonDocument(normalizePreflightReport(guiReport)).toJson(QJsonDocument::Compact),
             QJsonDocument(normalizePreflightReport(cliReport)).toJson(QJsonDocument::Compact));

    const QString historyPath =
        QDir(QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    const auto editorAuditIt = std::find_if(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                                            { return event.kind == pdf::PDFOperationHistoryEventKind::PreflightRun &&
                                                     event.status == pdf::PDFOperationHistoryStatus::Accepted &&
                                                     event.operatorIdentity == QStringLiteral("LoopEditor"); });
    QVERIFY(editorAuditIt != events.cend());
    QCOMPARE(editorAuditIt->effectiveProfileDigest, cliReport.value(QStringLiteral("effective_profile_digest")).toString());
    QCOMPARE(editorAuditIt->resultSummary.value(QStringLiteral("coverage_scope")).toObject().value(QStringLiteral("scope_restrictions")).toObject(),
             cliScope);
    QVERIFY(history.verify().verified);
}

void EditorHostTest::importValidProfileAddsDigest()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    pdf::PDFSettings::setSettingsPath(temp.path());
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);

    const QString sourcePath = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
    QFile bundled(sourcePath);
    QVERIFY(bundled.open(QIODevice::ReadOnly));
    const QJsonObject profile = QJsonDocument::fromJson(bundled.readAll()).object();
    const QString expectedDigest = profile.value(QStringLiteral("digest")).toString();
    QVERIFY(!expectedDigest.isEmpty());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importPreflightProfileFileUrl(QUrl::fromLocalFile(sourcePath)));

    const QString expectedId = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                   .filePath(QStringLiteral("profiles/loop-default.json"));
    bool found = false;
    for (const QVariant& entry : host.preflightProfiles())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("id")).toString() == expectedId)
        {
            found = true;
            QCOMPARE(item.value(QStringLiteral("digest")).toString(), expectedDigest);
            QVERIFY(item.value(QStringLiteral("valid")).toBool());
            break;
        }
    }
    QVERIFY(found);
}

void EditorHostTest::importDigestMismatchProfileIsRejected()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QJsonObject profile = pdf::exportPreflightProfile(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bad-profile") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Bad") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 300 } } } } });
    profile.insert(QStringLiteral("digest"), QString(64, QLatin1Char('a')));
    const QString sourcePath = temp.filePath(QStringLiteral("bad-profile.json"));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.write(QJsonDocument(profile).toJson(QJsonDocument::Compact));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(!host.importPreflightProfileFileUrl(QUrl::fromLocalFile(sourcePath)));
}

void EditorHostTest::saveProfileForkRecordsDerivedFrom()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QString parentDigest;
    for (const QVariant& entry : host.preflightProfiles())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("id")) == host.selectedPreflightProfileId())
        {
            parentDigest = item.value(QStringLiteral("digest")).toString();
            break;
        }
    }
    QVERIFY(!parentDigest.isEmpty());

    QVERIFY(host.beginPreflightProfileEdit());
    QVERIFY(host.setPreflightCheckField(QStringLiteral("image-resolution"), QStringLiteral("min_dpi"), 240));
    const QString destination = temp.filePath(QStringLiteral("forked-profile.json"));
    QVERIFY(host.savePreflightProfileEdit(host.preflightProfileDraftVersion(), QUrl::fromLocalFile(destination)));

    QFile forkedFile(destination);
    QVERIFY(forkedFile.open(QIODevice::ReadOnly));
    const QJsonObject forked = QJsonDocument::fromJson(forkedFile.readAll()).object();
    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(forked, destination);
    QVERIFY2(imported.ok, qPrintable(imported.errorMessage));
    QCOMPARE(forked.value(QStringLiteral("derived_from")).toObject().value(QStringLiteral("digest")).toString(), parentDigest);
    QVERIFY(forked.value(QStringLiteral("digest")).toString() != parentDigest);

    const QByteArray roundTrip = pdf::serializePreflightProfileBytes(imported.profile);
    const pdf::PreflightProfileImportResult reimported = pdf::importPreflightProfile(QJsonDocument::fromJson(roundTrip).object());
    QVERIFY(reimported.ok);
    QCOMPARE(roundTrip, pdf::serializePreflightProfileBytes(reimported.profile));
}

void EditorHostTest::openLargeDocument()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    constexpr int pageCount = 1000;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 612, 792));
    }

    const QString path = directory.filePath(QStringLiteral("large.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    QElapsedTimer openTimer;
    openTimer.start();

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 90000);
    const qint64 openToFirstViewMs = openTimer.elapsed();

    QCOMPARE(host.pageCount(), pageCount);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionGoToDocumentEnd")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionGoToDocumentEnd")) != 0);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), pageCount - 1, 15000);

    // --- Interaction revision and surface fencing under load (gh-363) ---
    // EditorHost owns DocumentViewSession privately; sessionForTest() is the
    // minimal test-only accessor that lets the shell stress test observe
    // revision/viewport/surfaces/interaction without breaking QML encapsulation.
    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QVERIFY(session->revisionSource() != nullptr);
    QVERIFY(session->surfaces() != nullptr);
    QVERIFY(session->interaction() != nullptr);

    // Ensure the viewport is in a deterministic state for hit-testing. The
    // host was already given 1024x768 above; re-apply to guarantee
    // requestGeneration is stable before we snapshot it.
    // Note: setViewportGeometry is idempotent when the values are unchanged,
    // so this does not bump generation if already set.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);

    // The shell's Quick host without a LoopCanvasItem leaves
    // DocumentViewSession::hitTest() empty (EditorHost::bindCanvas() is the
    // only place that adds PageBoxHitTestSource). For a fencing proof we need
    // a source that can produce overlay-only hover transitions. Inject a
    // scripted finding that covers a small rect around the viewport center so
    // pointer sweeps can enter/leave it without advancing generation.
    pdfinteraction::ViewportController& viewport = session->viewport();
    QVERIFY(viewport.pageCount() == pageCount);
    QRect placed = viewport.placedPageRect(viewport.currentPage());
    // Fallback to page 0 if current page placement is empty (should not happen after navigation).
    if (placed.isEmpty())
    {
        placed = viewport.placedPageRect(0);
    }
    QVERIFY(!placed.isEmpty());

    const QPoint viewportCenter = placed.center();
    const std::optional<QPointF> optCenterPage = viewport.viewportToPagePoint(viewportCenter, viewport.currentPage());
    // Center of a 612x792 page is approx 306x396. Use that as fallback if the
    // inverse mapping fails (e.g., due to rounding).
    const QPointF centerPagePoint = optCenterPage.value_or(QPointF(306.0, 396.0));

    // Finding bounds: 30x30 square centered at the page center point, guaranteed
    // to be inside the page's media box and inside the viewport after mapping.
    const QRectF findingBounds(centerPagePoint.x() - 15.0, centerPagePoint.y() - 15.0, 30.0, 30.0);
    ScriptedHitTestSource scripted;
    scripted.targets.push_back(makeFindingTarget(QStringLiteral("stress-finding-1"), findingBounds, viewport.currentPage()));
    // Also expose the page-box source for completeness; edge hits are another
    // overlay-only path but not required for the assertion.
    session->hitTest()->addSource(&scripted);
    session->hitTest()->addSource(&session->pageBoxSource());

    const pdf::PDFRevisionIdentity revisionBefore = session->revisionSource()->currentRevision();
    const quint64 generationBefore = viewport.requestGeneration();
    const int requestedBefore = session->surfaces()->counters().requested;
    const qint64 admittedHighWaterBefore = session->surfaces()->counters().admittedBytesHighWater;

    QSignalSpy overlaySpy(session->interaction(), &pdfinteraction::InteractionController::overlayFrameChanged);
    QSignalSpy viewportSpy(session->interaction(), &pdfinteraction::InteractionController::viewportChanged);
    QSignalSpy hoverSpy(session->interaction(), &pdfinteraction::InteractionController::hoverChanged);
    QVERIFY(overlaySpy.isValid());
    QVERIFY(viewportSpy.isValid());

    // Helper: page point -> viewport pixel via the same matrix the surfaces use.
    // This keeps the test's arithmetic from diverging from the controller's.
    auto viewportPointFor = [&viewport](QPointF pagePoint, int pageIndex) -> QPoint
    {
        const QTransform matrix = viewport.pagePointToViewportMatrix(pageIndex);
        return matrix.map(pagePoint).toPoint();
    };

    const int currentPageIndex = viewport.currentPage();
    // Perform 80 pointer Move sweeps that alternately hit and miss the scripted
    // finding. Hover changes are overlay-only: they must produce overlay frames
    // but never advance requestGeneration or surface demand.
    constexpr int sweepSteps = 80;
    for (int step = 0; step < sweepSteps; ++step)
    {
        // Sweep horizontally across the finding: offset -40..+39 scaled by 2,
        // so the pointer travels ~160 page units centered on the finding.
        // Steps ~33..47 hit the 30-unit finding, others miss, yielding at least
        // 2 hover transitions (enter + leave) and thus >=2 overlay frames.
        const qreal offset = qreal(step) - 40.0;
        const QPointF pagePoint(centerPagePoint.x() + offset * 2.0, centerPagePoint.y());
        const QPoint viewportPoint = viewportPointFor(pagePoint, currentPageIndex);
        session->interaction()->handlePointer(makeMoveIntent(viewportPoint, quint64(step + 1)));
    }

    // Fencing holds: no document revision change, no viewport generation bump,
    // no new surface requests, no renderer invocation via surfaces. The sweep
    // did produce overlay work.
    QCOMPARE(session->revisionSource()->currentRevision(), revisionBefore);
    QCOMPARE(viewport.requestGeneration(), generationBefore);
    QCOMPARE(session->surfaces()->counters().requested, requestedBefore);
    // Admitted high water should not have grown due to overlay-only input.
    QCOMPARE(session->surfaces()->counters().admittedBytesHighWater, admittedHighWaterBefore);
    QCOMPARE(viewportSpy.size(), 0);
    // At least enter + leave of the finding, plus potentially Page fallback
    // transitions. The exact count depends on hit-test tie-break but is >=2.
    QVERIFY(overlaySpy.size() >= 2);
    // hoverChanged is emitted only when the hovered target actually changes;
    // sweeping across the finding should have produced at least 2 changes.
    QVERIFY(hoverSpy.size() >= 2);

    // Also verify that surfaces remain fenced at the coordinator's generation:
    // overlay-only hover must not have advanced the PageSurfaceCoordinator's
    // own generation (which tracks demand supersession). The coordinator's
    // generation is independent of the viewport's but is similarly only
    // advanced by requestSurfaces/invalidate, not by interaction.
    const quint64 coordinatorGenerationBefore = session->surfaces()->generation();
    // A second smaller sweep to ensure coordinator generation still stable.
    for (int step = 0; step < 20; ++step)
    {
        const QPointF pagePoint(centerPagePoint.x() + qreal(step), centerPagePoint.y() + 10.0);
        const QPoint viewportPoint = viewportPointFor(pagePoint, currentPageIndex);
        session->interaction()->handlePointer(makeMoveIntent(viewportPoint, quint64(sweepSteps + step + 1)));
    }
    QCOMPARE(session->surfaces()->generation(), coordinatorGenerationBefore);
    QCOMPARE(viewport.requestGeneration(), generationBefore);
    QCOMPARE(session->revisionSource()->currentRevision(), revisionBefore);

    // Optional envelope: when LOOP_STRESS_ENVELOPE is set, record
    // PDFWorkloadEnvelope identity and timings for observability. This is
    // opt-in so CI without the env var does not pay the cost of JSON
    // assertions, but when enabled it proves the 1k-page shell open is
    // measurable and the envelope contract holds.
    const QByteArray envelopeEnv = qgetenv("LOOP_STRESS_ENVELOPE");
    if (!envelopeEnv.isEmpty())
    {
        const bool envelopeEnabled = envelopeEnv == QByteArrayLiteral("1") || envelopeEnv.toLower() == QByteArrayLiteral("true");
        if (envelopeEnabled)
        {
            pdf::PDFWorkloadEnvelope envelope;
            envelope.identity = pdf::PDFRunIdentity::capture();
            envelope.family = QStringLiteral("shell-stress-1k");
            envelope.status = QStringLiteral("complete");
            envelope.pageCount = pageCount;
            envelope.openToFirstViewMs = openToFirstViewMs;
            envelope.rssHighWaterBytes = pdf::PDFWorkloadEnvelope::currentRssHighWaterBytes();
            envelope.cacheHighWaterBytes = session->surfaces()->counters().admittedBytesHighWater;
            envelope.pressureShedCount = session->surfaces()->counters().shed;
            envelope.elapsedMs = openTimer.elapsed();
            envelope.prefetchShed = session->surfaces()->counters().shed > 0;
            // Interaction slot was held throughout the sweep without blocking.
            envelope.interactionSlotHeld = true;

            const QJsonObject json = envelope.toJson();
            // Contract: family, page_count, open_to_first_view_ms, rss, cache,
            // pressure_shed_count, identity must be present.
            QCOMPARE(json.value(QStringLiteral("family")).toString(), QStringLiteral("shell-stress-1k"));
            QCOMPARE(json.value(QStringLiteral("page_count")).toInt(), pageCount);
            QVERIFY(json.contains(QStringLiteral("open_to_first_view_ms")));
            QVERIFY(!json.value(QStringLiteral("open_to_first_view_ms")).isNull());
            QVERIFY(json.value(QStringLiteral("open_to_first_view_ms")).toInt() >= 0);
            QVERIFY(json.contains(QStringLiteral("rss_high_water_bytes")));
            QVERIFY(json.contains(QStringLiteral("cache_high_water_bytes")));
            QVERIFY(json.contains(QStringLiteral("pressure_shed_count")));
            QVERIFY(json.contains(QStringLiteral("identity")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("commit")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("qt")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("os")));
            QVERIFY(json.value(QStringLiteral("status")).toString() == QStringLiteral("complete"));

            qDebug() << "PDFWorkloadEnvelope shell-stress-1k"
                     << QJsonDocument(json).toJson(QJsonDocument::Compact);
            qDebug() << "openToFirstViewMs" << openToFirstViewMs << "rssHighWater"
                     << envelope.rssHighWaterBytes << "cacheHighWater" << envelope.cacheHighWaterBytes
                     << "shed" << envelope.pressureShedCount;
        }
    }

    // Cleanup: remove scripted sources so teardown does not leave dangling
    // pointers (scripted is stack-allocated). DocumentViewSession will be
    // destroyed with EditorHost.
    session->hitTest()->clearSources();
}

// ---------------------------------------------------------------------------
// Workspace surfaces (#586)
// ---------------------------------------------------------------------------

void EditorHostTest::fixWorkspacePresentsIdleLifecycleAndRefusesToArm()
{
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());

    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QVERIFY(!host.fixLifecycleSummary().trimmed().isEmpty());
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.fixRollbackAvailable());
    QVERIFY(!host.fixRollbackSummary().trimmed().isEmpty());
    QVERIFY(host.fixRollbackPoints().isEmpty());
    QVERIFY(!host.fixPreview().value(QStringLiteral("available")).toBool());
    QVERIFY(!host.fixPreview().value(QStringLiteral("incomplete")).toBool());

    // Nothing may be approved, rejected, executed, rolled back or inspected before a plan
    // exists for this revision.
    QVERIFY(!host.approveActionListPlan());
    QVERIFY(!host.rejectActionListPlan());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.inspectActionListStep(0));
    QVERIFY(!host.requestFixRollback(QStringLiteral("not-a-recorded-revision")));

    // The lifecycle is always renderable, carries words as well as a shape, and never claims
    // a pass.
    const QVariantMap visual = host.fixLifecycleVisual();
    QCOMPARE(visual.value(QStringLiteral("kind")).toString(), QStringLiteral("NotChecked"));
    QCOMPARE(visual.value(QStringLiteral("icon")).toString(), QStringLiteral("Outline"));
    QVERIFY(!visual.value(QStringLiteral("accessibleName")).toString().trimmed().isEmpty());

    // The identity every Fix affordance binds to exists even before a plan does, so a pane
    // never has to invent one.
    const QVariantMap identity = host.fixPlanIdentity();
    QCOMPARE(identity.value(QStringLiteral("lifecycleStateName")).toString(), QStringLiteral("idle"));
    QCOMPARE(identity.value(QStringLiteral("reviewDecision")).toString(), QStringLiteral("none"));
    QCOMPARE(identity.value(QStringLiteral("planIsCurrent")).toBool(), false);
    QCOMPARE(identity.value(QStringLiteral("executionArmed")).toBool(), false);
}

void EditorHostTest::fixReviewBindsToThePlannedDigestAndTheCurrentRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("gh-586-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("gh-586-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 9 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    // The catalog identifies a recipe by the file that defines it, and importing selects it.
    QVERIFY(!host.selectedActionListRecipeId().isEmpty());

    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // The Inspect workspace's corrective intent lands on a recipe that really runs the
    // operation, and plans nothing by itself.
    const QString recipeId = host.selectedActionListRecipeId();
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QCOMPARE(host.selectedActionListRecipeId(), recipeId);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QVERIFY(!host.selectActionListRecipeForOperation(QStringLiteral("no-such-operation")));
    QVERIFY(!host.selectActionListRecipeForOperation(QString()));

    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);

    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    // The plan is bound to the revision it was produced for, and the operator's approval is
    // bound to that exact plan digest.
    const QVariantMap planned = host.fixPlanIdentity();
    const QString planDigest = planned.value(QStringLiteral("planDigest")).toString();
    QVERIFY(!planDigest.isEmpty());
    QCOMPARE(planned.value(QStringLiteral("planIsCurrent")).toBool(), true);
    QCOMPARE(planned.value(QStringLiteral("reviewDecision")).toString(), QStringLiteral("none"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());

    QVERIFY(host.approveActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(host.fixExecutionArmed());
    const QVariantMap approved = host.fixPlanIdentity();
    QCOMPARE(approved.value(QStringLiteral("reviewDecision")).toString(), QStringLiteral("approved"));
    QCOMPARE(approved.value(QStringLiteral("reviewedPlanDigest")).toString(), planDigest);
    QCOMPARE(approved.value(QStringLiteral("executionArmed")).toBool(), true);

    // Rejecting is the operator's own decision and leaves the plan visible for inspection.
    QVERIFY(host.rejectActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("rejected"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());

    host.replanActionList();
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QVERIFY(host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString().isEmpty());

    // A revision change may not inherit the review: the plan goes stale, the approval stops
    // arming execution, and nothing may be approved against the superseded digest.
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.fixExecutionArmed());

    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("stale"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.approveActionListPlan());
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("planIsCurrent")).toBool(), false);
    QVERIFY(!host.fixLifecycleSummary().trimmed().isEmpty());
}

void EditorHostTest::moveSelectionProposesAPageBoxMoveInTheFixWorkspace()
{
    const QString commandId = QStringLiteral("actionMoveSelection");
    const auto moveParameters = [](const QString& kind, double dx, double dy)
    {
        return QVariantMap{ { QStringLiteral("targetKind"), kind },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), dx },
                            { QStringLiteral("dy"), dy } };
    };

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("move.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("issue-104-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("issue-104-test") },
                                       { QStringLiteral("name"), QStringLiteral("Move a page box") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("move") },
                                             { QStringLiteral("operation"), QStringLiteral("translate-page-box") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("box"), QStringLiteral("trim") },
                                                            { QStringLiteral("page_index"), 0 },
                                                            { QStringLiteral("dx"), 1 },
                                                            { QStringLiteral("dy"), 1 } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(!host.isCommandEnabled(commandId));
    QCOMPARE(host.invokeCommand(commandId, moveParameters(QStringLiteral("PageBox"), 4.0, 5.0)), quint64(0));

    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.isCommandEnabled(commandId));

    // Only a page box is a document edit, and a zero move is not one.
    host.setWorkspace(EditorHost::Document);
    QVERIFY(host.invokeCommand(commandId, moveParameters(QStringLiteral("Finding"), 4.0, 5.0)) != 0);
    QVERIFY(host.invokeCommand(commandId, moveParameters(QStringLiteral("PageBox"), 0.0, 0.0)) != 0);
    QCOMPARE(host.workspace(), EditorHost::Document);

    QVERIFY(host.invokeCommand(commandId, moveParameters(QStringLiteral("PageBox"), 4.0, 5.0)) != 0);
    QCOMPARE(host.workspace(), EditorHost::Fix);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QCOMPARE(host.actionListBindings().size(), 4);
    for (const QVariant& binding : host.actionListBindings())
    {
        const QVariantMap entry = binding.toMap();
        if (entry.value(QStringLiteral("name")).toString() == QStringLiteral("dx"))
        {
            QCOMPARE(entry.value(QStringLiteral("value")).toDouble(), 4.0);
        }
    }
}

QString shippedTranslatePageBoxRecipeId()
{
    return QStringLiteral(":/loop/builtin-recipe-translate-page-box.json");
}

bool writeTranslatePageBoxRecipe(const QString& path, const QString& id, const QJsonValue& pageIndex)
{
    QFile recipe(path);
    if (!recipe.open(QIODevice::WriteOnly))
    {
        return false;
    }
    const QJsonDocument document(QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), id },
        { QStringLiteral("steps"),
          QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("move") },
                                   { QStringLiteral("operation"), QStringLiteral("translate-page-box") },
                                   { QStringLiteral("params"),
                                     QJsonObject{ { QStringLiteral("box"), QStringLiteral("trim") },
                                                  { QStringLiteral("page_index"), pageIndex },
                                                  { QStringLiteral("dx"), 2 },
                                                  { QStringLiteral("dy"), 3 } } } } } } });
    return recipe.write(document.toJson(QJsonDocument::Compact)) >= 0;
}

// A page whose trim box sits inside its media box. The default profile requires 9 pt of bleed
// around the trim, and with no BleedBox that bleed is the media box, so the 20 pt inset keeps
// 9 pt on every edge after the 2/3 point move.
bool writeTrimmedPageDocument(const QString& path)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setPageTrimBox(page, QRectF(20, 20, 160, 160));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    return static_cast<bool>(writer.write(path, &document, true));
}

/// A page whose trim box sits inside its media box, carrying a signature
/// dictionary so the save-policy world treats the document as signed. The blank
/// page passes the default profile (no fonts, no color, 20 pt of bleed), which
/// is what lets a successful governed publish be observed.
bool writeSignedTrimmedPageDocument(const QString& path)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setPageTrimBox(page, QRectF(20, 20, 160, 160));

    const pdf::PDFObjectReference signature = builder.createSignatureDictionary(
        QByteArrayLiteral("Adobe.PPKLite"),
        QByteArrayLiteral("adbe.pkcs7.detached"),
        QByteArrayLiteral("signed-placeholder"),
        QDateTime::currentDateTimeUtc(),
        0);
    const pdf::PDFObjectReference field = builder.createFormFieldSignature(
        QStringLiteral("LoopSignature"), {}, signature);
    builder.createInvisibleFormFieldWidget(field, page);
    builder.createAcroForm({ field });

    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    return static_cast<bool>(writer.write(path, &document, true));
}

/// The committed signed fixture, located through the same source-tree
/// environment variable UnitTestsIncrementalSave uses.
QString signedFixturePath(const QString& fileName = QStringLiteral("signed-incremental-base.pdf"))
{
    const QString root = qEnvironmentVariable("LOOP_FIXTURE_DATA_DIR");
    return root.isEmpty() ? QStringLiteral("testdata/signatures/%1").arg(fileName)
                          : QStringLiteral("%1/signatures/%2").arg(root, fileName);
}

/// Two pages, one Text annotation each, with an author, contents and subject.
bool writeAnnotationFixture(const QString& path)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference firstPage = builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFObjectReference secondPage = builder.appendPage(QRectF(0, 0, 200, 200));

    const auto addTextAnnotation = [&builder](const pdf::PDFObjectReference& pageReference,
                                              const QString& author,
                                              const QString& contents,
                                              const QRectF& rect)
    {
        pdf::PDFDictionary annotation;
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("Annot"));
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Text"));
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("Rect"),
                            pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                                pdf::PDFObject::createReal(rect.left()),
                                pdf::PDFObject::createReal(rect.top()),
                                pdf::PDFObject::createReal(rect.right()),
                                pdf::PDFObject::createReal(rect.bottom()) })));
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("T"), pdf::PDFObject::createString(author.toUtf8()));
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createString(contents.toUtf8()));
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("Subj"), pdf::PDFObject::createString("Comment"));
        annotation.addEntry(pdf::PDFInplaceOrMemoryString("P"), pdf::PDFObject::createReference(pageReference));
        const pdf::PDFObjectReference annotationReference = builder.addObject(
            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(annotation))));

        pdf::PDFDictionary pageAnnots;
        pageAnnots.addEntry(pdf::PDFInplaceOrMemoryString("Annots"),
                            pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                                pdf::PDFObject::createReference(annotationReference) })));
        builder.appendTo(pageReference,
                         pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageAnnots))));
    };

    addTextAnnotation(firstPage, QStringLiteral("Reviewer"), QStringLiteral("Please check the margin"),
                      QRectF(20.0, 40.0, 60.0, 80.0));
    addTextAnnotation(secondPage, QStringLiteral("Client"), QStringLiteral("Approve page two"),
                      QRectF(100.0, 120.0, 60.0, 40.0));

    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    return static_cast<bool>(writer.write(path, &document, true));
}

/// Copies the signed fixture with one byte changed inside the signed range
/// (the producer string), which is exactly a digest that no longer matches.
bool writeSignatureTamperedCopy(const QString& sourcePath, const QString& destinationPath)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly))
    {
        return false;
    }
    QByteArray bytes = source.readAll();
    const int producer = bytes.indexOf("pyHanko");
    if (producer < 0)
    {
        return false;
    }
    bytes[producer + 6] = 'n';   // "pyHanko" -> "pyHankn"

    QFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly))
    {
        return false;
    }
    return destination.write(bytes) == bytes.size();
}

/// Copies the signed fixture with bytes appended after the signature, which is
/// a signature that still verifies but no longer covers the whole file.
bool writeSignatureAppendedCopy(const QString& sourcePath, const QString& destinationPath)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly))
    {
        return false;
    }
    QByteArray bytes = source.readAll();
    bytes.append("\n% reviewed after signing\n");

    QFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly))
    {
        return false;
    }
    return destination.write(bytes) == bytes.size();
}

void EditorHostTest::shippedRecipeIsListedButNeverSelectedByDefault()
{
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    bool listed = false;
    for (const QVariant& entry : host.actionListRecipes())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("id")).toString() == shippedTranslatePageBoxRecipeId())
        {
            listed = true;
            QVERIFY(item.value(QStringLiteral("valid")).toBool());
            QVERIFY(item.value(QStringLiteral("builtIn")).toBool());
        }
    }
    QVERIFY(listed);
    QVERIFY(host.selectedActionListRecipeId().isEmpty());
}

void EditorHostTest::shippedPageBoxRecipeRunsDefaultMoveThroughPlanApprovalAndRevalidation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString documentPath = directory.filePath(QStringLiteral("default-move.pdf"));
    QVERIFY(writeTrimmedPageDocument(documentPath));
    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // A drag proposes the move, and the shipped recipe binds to it with no import. Nothing is
    // selected before the drag, so the shipped recipe is never a silent default.
    QVERIFY(host.selectedActionListRecipeId().isEmpty());
    const QVariantMap move{ { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), 2.0 },
                            { QStringLiteral("dy"), 3.0 } };
    QVERIFY(host.invokeCommand(QStringLiteral("actionMoveSelection"), move) != 0);
    QCOMPARE(host.workspace(), EditorHost::Fix);
    QCOMPARE(host.selectedActionListRecipeId(), shippedTranslatePageBoxRecipeId());
    QCOMPARE(host.actionListBindings().size(), 4);

    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    const QString approvedDigest = host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString();
    QVERIFY(!approvedDigest.isEmpty());

    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.fixExecutionArmed());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("executing"), 120000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("succeeded"));

    // The published artifact is bound to the approved plan and is not the as-received input.
    const QVariantMap signOff = host.fixSignOff();
    QCOMPARE(signOff.value(QStringLiteral("planDigest")).toString(), approvedDigest);
    const QString publishedSha256 = signOff.value(QStringLiteral("publishedSha256")).toString();
    QCOMPARE(publishedSha256.size(), 64);
    QVERIFY(publishedSha256 != QString::fromLatin1(sourceDigest));
    QCOMPARE(fileDigest(documentPath), sourceDigest);
}

void EditorHostTest::operatorRecipeWinsOverShippedRecipe()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString documentPath = directory.filePath(QStringLiteral("operator-move.pdf"));
    QVERIFY(writeTrimmedPageDocument(documentPath));
    const QString recipePath = directory.filePath(QStringLiteral("operator-move.json"));
    QVERIFY(writeTranslatePageBoxRecipe(recipePath, QStringLiteral("operator-move"), 0));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // A valid operator recipe that offers the operation is selected ahead of the shipped one, and
    // it is named on the surface so the operator can see which recipe the plan belongs to.
    const QVariantMap move{ { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), 2.0 },
                            { QStringLiteral("dy"), 3.0 } };
    QVERIFY(host.invokeCommand(QStringLiteral("actionMoveSelection"), move) != 0);
    QVERIFY(!host.selectedActionListRecipeId().isEmpty());
    QVERIFY(host.selectedActionListRecipeId() != shippedTranslatePageBoxRecipeId());
    QVERIFY(host.selectedActionListRecipeId().endsWith(QStringLiteral("operator-move.json")));
}

void EditorHostTest::invalidOperatorRecipeStopsMoveWithoutShippedFallback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString documentPath = directory.filePath(QStringLiteral("broken-move.pdf"));
    QVERIFY(writeTrimmedPageDocument(documentPath));
    // The operator recipe offers the operation with a page index the schema refuses, so it is
    // invalid. It sits in the recipes directory before the host starts, as an imported copy would.
    const QString brokenPath = QDir(m_fixtureRecipes->path()).filePath(QStringLiteral("broken-move.json"));
    QVERIFY(writeTranslatePageBoxRecipe(brokenPath, QStringLiteral("broken-move"), QStringLiteral("first")));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // The move is refused and nothing is selected: the shipped recipe must not run in place of
    // the operator's invalid one.
    const QVariantMap move{ { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), 2.0 },
                            { QStringLiteral("dy"), 3.0 } };
    host.invokeCommand(QStringLiteral("actionMoveSelection"), move);
    QVERIFY(host.selectedActionListRecipeId().isEmpty());
    QVERIFY(!host.selectActionListRecipeForOperation(QStringLiteral("translate-page-box")));
    QVERIFY(host.selectedActionListRecipeId().isEmpty());

    bool brokenIsInvalid = false;
    for (const QVariant& entry : host.actionListRecipes())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("name")).toString() == QStringLiteral("broken-move"))
        {
            brokenIsInvalid = !item.value(QStringLiteral("valid")).toBool();
        }
    }
    QVERIFY(brokenIsInvalid);
}

void EditorHostTest::completedDragOfARefusedKindIsReportedAndChangesNothing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("refused-drag.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QVERIFY(session->interaction() != nullptr);

    host.setWorkspace(EditorHost::Document);
    const auto revisionBefore = session->facade().currentRevision();
    QSignalSpy refusedSpy(&host, &EditorHost::dragRefused);

    const struct
    {
        pdfinteraction::InteractionTargetKind kind;
        const char* name;
    } refusedKinds[] = { { pdfinteraction::InteractionTargetKind::Finding, "finding" },
                         { pdfinteraction::InteractionTargetKind::Guide, "guide" },
                         { pdfinteraction::InteractionTargetKind::DragHandle, "drag-handle" } };
    for (const auto& refused : refusedKinds)
    {
        pdfinteraction::DragSession dragged;
        dragged.target.kind = refused.kind;
        dragged.target.pageIndex = 0;
        dragged.target.id = QStringLiteral("subject");
        dragged.pageDelta = QPointF(4.0, 5.0);
        dragged.exceededThreshold = true;

        const int before = refusedSpy.count();
        Q_EMIT session->interaction()->dragCompleted(dragged);

        QCOMPARE(refusedSpy.count(), before + 1);
        QCOMPARE(refusedSpy.last().at(0).toString(), QString::fromLatin1(refused.name));
        QCOMPARE(refusedSpy.last().at(1).toString(), QStringLiteral("subject"));
        QCOMPARE(host.workspace(), EditorHost::Document);
        QCOMPARE(session->facade().currentRevision(), revisionBefore);
    }

    // A page box is admitted, so it is never reported as refused.
    pdfinteraction::DragSession boxDrag;
    boxDrag.target.kind = pdfinteraction::InteractionTargetKind::PageBox;
    boxDrag.target.pageIndex = 0;
    boxDrag.target.id = QStringLiteral("trim");
    boxDrag.pageDelta = QPointF(4.0, 5.0);
    boxDrag.exceededThreshold = true;
    const int refusalsBeforeBox = refusedSpy.count();
    Q_EMIT session->interaction()->dragCompleted(boxDrag);
    QCOMPARE(refusedSpy.count(), refusalsBeforeBox);
}

void EditorHostTest::fixJourneyPublishesOnlyAnApprovedPlanBoundToTheDisplayedIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("plan-approval-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("plan-approval-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 9 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    // What the operator is shown is what an approval binds: one plan digest on both the
    // identity surface and the preview, against one document revision.
    const QVariantMap displayed = host.fixPlanIdentity();
    const QString displayedDigest = displayed.value(QStringLiteral("planDigest")).toString();
    QVERIFY(!displayedDigest.isEmpty());
    QCOMPARE(displayed.value(QStringLiteral("planIsCurrent")).toBool(), true);
    QVERIFY(!displayed.value(QStringLiteral("plannedRevision")).toString().isEmpty());
    const QVariantMap preview = host.fixPreview();
    QCOMPARE(preview.value(QStringLiteral("planDigest")).toString(), displayedDigest);
    QCOMPARE(preview.value(QStringLiteral("planIsCurrent")).toBool(), true);

    // Unapproved: a plan nobody has approved cannot start a run, and nothing is published.
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("preview-ready"));
    QVERIFY(!host.fixRecheck().value(QStringLiteral("available")).toBool());
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // Denied: the operator's own refusal is honoured, and it also starts nothing.
    QVERIFY(host.rejectActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("rejected"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.fixRecheck().value(QStringLiteral("available")).toBool());
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // Stale: reopening the document moves the revision, so the plan belongs to an earlier one.
    // The surface names the revision its evidence belongs to instead of looking clear, the plan
    // reports itself as no longer current, and no run may start against it.
    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("stale"));
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("planIsCurrent")).toBool(), false);
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.approveActionListPlan());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.fixRecheck().value(QStringLiteral("available")).toBool());
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // The operator now plans against the revision that is open and approves it: the approval is
    // armed and bound to the digest this plan actually has.
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    const QString approvedDigest = host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString();
    QVERIFY(!approvedDigest.isEmpty());
    QVERIFY(host.approveActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(host.fixExecutionArmed());
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("reviewedPlanDigest")).toString(), approvedDigest);

    // An approval belongs to the plan it was given for: planning again against the same revision
    // clears the review, so the run is refused until the operator approves the plan that is
    // displayed now, and nothing is published in between. Mutation-probed: deleting the
    // clearFixReview() call that runs when a plan is accepted makes this step fail, so the refusal
    // is enforced by that line rather than by an accident of state. Two further clauses are
    // defensive against a state this journey offers no path to, and removing either leaves this
    // fixture green: the reviewed-digest equality inside fixLifecycleStateName() and the
    // fixPlanIsCurrent() clause in approveActionListPlan(), both already decided by the state
    // checks that run before them.
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(fileDigest(documentPath), sourceDigest);
    QVERIFY(host.approveActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(host.fixExecutionArmed());

    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("executing"), 120000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("succeeded"));

    // Completion presents the revalidated result and the artifact Core signed, distinct
    // from the as-received input and bound to the plan the operator approved.
    const QVariantMap recheck = host.fixRecheck();
    QVERIFY(recheck.value(QStringLiteral("available")).toBool());
    const QString verdictState = recheck.value(QStringLiteral("verdictState")).toString();
    QVERIFY2(verdictState == QStringLiteral("pass") || verdictState == QStringLiteral("incomplete") ||
                 verdictState == QStringLiteral("fail"),
             qPrintable(verdictState));
    QVERIFY2(verdictState != QStringLiteral("fail"), qPrintable(verdictState));
    const QVariantMap signOff = host.fixSignOff();
    QCOMPARE(signOff.value(QStringLiteral("planDigest")).toString(), approvedDigest);
    const QString publishedSha256 = signOff.value(QStringLiteral("publishedSha256")).toString();
    QCOMPARE(publishedSha256.size(), 64);
    for (const QChar character : publishedSha256)
    {
        QVERIFY2(character.isDigit() || (character >= QLatin1Char('a') && character <= QLatin1Char('f')),
                 qPrintable(publishedSha256));
    }
    QVERIFY(publishedSha256 != QString::fromLatin1(sourceDigest));
    QVERIFY2(host.fixLifecycleSummary().contains(QStringLiteral("published")),
             qPrintable(host.fixLifecycleSummary()));

    const QString exportPath = directory.filePath(QStringLiteral("accepted-correction.pdf"));
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(exportPath), QStringLiteral("pdf"), 300, host.outputRevision(), true));
    QTRY_VERIFY_WITH_TIMEOUT(!host.outputBusy(), 10000);
    QVERIFY2(host.outputRecord().value(QStringLiteral("status")).toString() == QLatin1String("saved"), qPrintable(host.outputRecord().value(QStringLiteral("error")).toString()));
    QCOMPARE(QString::fromLatin1(fileDigest(exportPath)), publishedSha256);
    QCOMPARE(host.outputRecord().value(QStringLiteral("publicationReceipt")).toMap().value(QStringLiteral("published_sha256")).toString(), publishedSha256);

    // The as-received input is never the publication target.
    QCOMPARE(fileDigest(documentPath), sourceDigest);
    const QString publishedPath = host.governedArtifactPath();
    QVERIFY(publishedPath != documentPath);
    QCOMPARE(QString::fromLatin1(fileDigest(publishedPath)), publishedSha256);
    QVERIFY(host.historyUndoAvailable());
    QVERIFY(!host.historyRedoAvailable());
    const QVariantList recorded = host.revisionHistory();
    QVERIFY(!recorded.isEmpty());
    QVERIFY(std::any_of(recorded.cbegin(), recorded.cend(), [&approvedDigest](const QVariant& value)
                        { return value.toMap().value(QStringLiteral("receiptText")).toString().contains(approvedDigest); }));
    const QString beforeRevision = host.sessionForTest()->facade().currentRevision().toString();
    host.invokeCommand(QStringLiteral("actionZoom_In"));
    host.setWorkspace(EditorHost::LoopWorkspace::Inspect);
    QVERIFY(host.sessionForTest()->interaction());
    host.sessionForTest()->interaction()->selectTarget(makeFindingTarget(QStringLiteral("view-only-selection"), QRectF(0, 0, 20, 20)));
    QCOMPARE(host.sessionForTest()->facade().currentRevision().toString(), beforeRevision);
    QCOMPARE(host.revisionHistory(), recorded);
    QSignalSpy confirmation(&host, &EditorHost::historyNavigationRequested);
    host.invokeCommand(QStringLiteral("actionUndo"));
    QCOMPARE(confirmation.count(), 1);
    const QList<QVariant> undoIntent = confirmation.takeFirst();
    const QVariantMap undoParameters{ { QStringLiteral("rollbackId"), undoIntent[1] },
                                      { QStringLiteral("revision"), undoIntent[2] } };
    host.invokeCommand(QStringLiteral("actionUndo"), QVariantMap{ { QStringLiteral("rollbackId"), undoIntent[1] },
                                                                  { QStringLiteral("revision"), QStringLiteral("stale") } });
    QCOMPARE(host.governedArtifactPath(), publishedPath);
    bool historyJobFinished = false;
    QObject::connect(&host.sessionForTest()->scheduler(), &pdf::PDFJobScheduler::jobFinished, &host,
                     [&historyJobFinished](const pdf::PDFJobSnapshot& snapshot)
                     { if (snapshot.operationId.startsWith(QStringLiteral("rollback."))) historyJobFinished = true; });
    host.invokeCommand(QStringLiteral("actionUndo"), undoParameters);
    QTRY_VERIFY_WITH_TIMEOUT(historyJobFinished, 15000);
    QCOMPARE(host.governedArtifactPath(), publishedPath);
    QCOMPARE(QString::fromLatin1(fileDigest(publishedPath)), publishedSha256);
    QCOMPARE(host.revisionHistory(), recorded);
    const QString historyProfilePath = directory.filePath(QStringLiteral("history-profile.json"));
    QFile historyProfile(historyProfilePath);
    QVERIFY(historyProfile.open(QIODevice::WriteOnly));
    historyProfile.write(QJsonDocument(QJsonObject{
                                           { QStringLiteral("schema_version"), 1 }, { QStringLiteral("id"), QStringLiteral("history-fixture") }, { QStringLiteral("version"), QStringLiteral("1.0.0") }, { QStringLiteral("name"), QStringLiteral("History fixture") }, { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("embedded-fonts") }, { QStringLiteral("severity"), QStringLiteral("error") } } } } })
                             .toJson());
    historyProfile.close();
    QVERIFY(host.importPreflightProfileFileUrl(QUrl::fromLocalFile(historyProfilePath)));
    historyJobFinished = false;
    host.invokeCommand(QStringLiteral("actionUndo"), undoParameters);
    QTRY_VERIFY_WITH_TIMEOUT(historyJobFinished, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(host.sessionForTest()->facade().source().path != documentPath && host.hasDocument(), 15000);
    const QString undoPath = host.sessionForTest()->facade().source().path;
    QCOMPARE(fileDigest(undoPath), sourceDigest);
    QCOMPARE(QString::fromLatin1(fileDigest(publishedPath)), publishedSha256);
    QVERIFY(!host.historyUndoAvailable());
    QVERIFY(host.historyRedoAvailable());
    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.historyRedoAvailable());
    host.invokeCommand(QStringLiteral("actionRedo"));
    QCOMPARE(confirmation.count(), 1);
    const QList<QVariant> redoIntent = confirmation.takeFirst();
    host.invokeCommand(QStringLiteral("actionRedo"), QVariantMap{ { QStringLiteral("rollbackId"), redoIntent[1] },
                                                                  { QStringLiteral("revision"), redoIntent[2] } });
    QTRY_VERIFY_WITH_TIMEOUT(host.sessionForTest()->facade().source().path != undoPath && host.hasDocument(), 60000);
    const QString redoPath = host.sessionForTest()->facade().source().path;
    QCOMPARE(QString::fromLatin1(fileDigest(redoPath)), publishedSha256);
    QVERIFY(host.historyUndoAvailable());
    QVERIFY(!host.historyRedoAvailable());
    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.historyUndoAvailable());
    QVERIFY(!host.historyRedoAvailable());
    pdf::PDFOperationHistoryStore persisted(redoPath + QStringLiteral(".loop-history/history.sqlite3"));
    QVERIFY(persisted.open());
    QVERIFY(persisted.verify().verified);
    const auto reopenedEvents = persisted.events();
    QVERIFY(std::any_of(reopenedEvents.cbegin(), reopenedEvents.cend(), [&approvedDigest](const auto& event)
                        { return event.status == pdf::PDFOperationHistoryStatus::Accepted &&
                                 event.resultSummary.value(QStringLiteral("governed")).toObject().value(QStringLiteral("receipt")).toObject().value(QStringLiteral("plan_digest")).toString() == approvedDigest; }));
    persisted.close();
    int correctionNumber = 0;
    const auto publishAnotherCorrection = [&]()
    {
        QFile nextRecipe(recipePath);
        if (!nextRecipe.open(QIODevice::WriteOnly))
            return false;
        nextRecipe.write(QJsonDocument(QJsonObject{
                                           { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                           { QStringLiteral("id"), QStringLiteral("history-next-%1").arg(++correctionNumber) },
                                           { QStringLiteral("name"), QStringLiteral("Next bleed correction") },
                                           { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                          { QStringLiteral("id"), QStringLiteral("bleed") }, { QStringLiteral("operation"), QStringLiteral("add-bleed") }, { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 5 + correctionNumber }, { QStringLiteral("mode"), QStringLiteral("mirror") }, { QStringLiteral("force"), true } } } } } } })
                             .toJson());
        nextRecipe.close();
        if (!host.importActionListRecipe(QUrl::fromLocalFile(recipePath)) || !host.validateActionListRecipe() ||
            !QTest::qWaitFor([&]
                             { return host.actionList()->property("validationReady").toBool(); }, 15000) ||
            !host.planActionList() || !QTest::qWaitFor([&]
                                                       { return host.fixLifecycleStateName() == QStringLiteral("preview-ready"); }, 15000) ||
            !host.approveActionListPlan() || !host.executeApprovedActionListPlan())
            return false;
        return QTest::qWaitFor([&]
                               { return host.fixLifecycleStateName() != QStringLiteral("executing"); }, 30000) &&
               host.fixLifecycleStateName() == QStringLiteral("succeeded");
    };
    const auto navigateHistory = [&](const QString& command, const QByteArray& expected)
    {
        const QString previousPath = host.sessionForTest()->facade().source().path;
        confirmation.clear();
        host.invokeCommand(command);
        if (confirmation.count() != 1)
            return false;
        const auto intent = confirmation.takeFirst();
        host.invokeCommand(command, QVariantMap{ { QStringLiteral("rollbackId"), intent[1] }, { QStringLiteral("revision"), intent[2] } });
        return QTest::qWaitFor([&]
                               { return host.hasDocument() && host.sessionForTest()->facade().source().path != previousPath; }, 15000) &&
               fileDigest(host.governedArtifactPath()) == expected;
    };
    QVERIFY2(publishAnotherCorrection(), qPrintable(host.fixLifecycleSummary() + QStringLiteral(" ") + host.actionList()->property("operatorSummary").toString()));
    const QByteArray secondDigest = fileDigest(host.governedArtifactPath());
    QVERIFY(secondDigest != publishedSha256.toLatin1());
    QVERIFY(navigateHistory(QStringLiteral("actionUndo"), publishedSha256.toLatin1()));
    QVERIFY(navigateHistory(QStringLiteral("actionUndo"), sourceDigest));
    QVERIFY(navigateHistory(QStringLiteral("actionRedo"), publishedSha256.toLatin1()));
    QVERIFY(host.historyRedoAvailable());
    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(navigateHistory(QStringLiteral("actionRedo"), secondDigest));
    QVERIFY(!host.historyRedoAvailable());
    QVERIFY(navigateHistory(QStringLiteral("actionUndo"), publishedSha256.toLatin1()));
    QVERIFY(host.historyRedoAvailable());
    QVERIFY2(publishAnotherCorrection(), qPrintable(host.fixLifecycleSummary() + QStringLiteral(" ") + host.actionList()->property("operatorSummary").toString()));
    QVERIFY(!host.historyRedoAvailable());
    QCOMPARE(QString::fromLatin1(fileDigest(publishedPath)), publishedSha256);
    qInfo().noquote() << "editor-history-fixture" << "source=" << sourceDigest
                      << "published=" << publishedSha256 << "undo=" << fileDigest(undoPath)
                      << "redo=" << fileDigest(redoPath) << "events=" << reopenedEvents.size();
}

void EditorHostTest::translatePageBoxRefusesAMoveThatIntroducesABlockingFinding()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // A 10 pt trim inset leaves 8 pt on the right and top edges after the +2/+3 pt move. The default
    // profile requires 9 pt of bleed, and with no BleedBox that bleed is the media box.
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setPageTrimBox(page, QRectF(10, 10, 180, 180));
    const QString documentPath = directory.filePath(QStringLiteral("tight-margin.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("tight-margin-move.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("tight-margin-move") },
                                       { QStringLiteral("name"), QStringLiteral("Tight margin move") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("move") },
                                             { QStringLiteral("operation"), QStringLiteral("translate-page-box") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("box"), QStringLiteral("trim") },
                                                            { QStringLiteral("page_index"), 0 },
                                                            { QStringLiteral("dx"), 2 },
                                                            { QStringLiteral("dy"), 3 } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    QVERIFY(!host.selectedActionListRecipeId().isEmpty());
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("executing"), 120000);

    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("failed"));
    QVERIFY(host.actionList()->property("operatorSummary").toString().contains(QStringLiteral("blocking findings")));
}

void EditorHostTest::fixRollbackReturnsToARecordedRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString sourcePath = directory.filePath(QStringLiteral("job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(sourcePath, &document, true));
    }

    // The recorded history a rollback returns to is written by the governed repair path; the
    // shell only reads it and appends the rolled-back event through Core.
    const QString repairedPath = directory.filePath(QStringLiteral("job-bleed.pdf"));
    {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        process.setProcessEnvironment(environment);
        process.start(QStringLiteral(PDFTOOL_EXECUTABLE_PATH),
                      { QStringLiteral("repair"),
                        sourcePath,
                        QStringLiteral("--operation"),
                        QStringLiteral("add-bleed"),
                        QStringLiteral("--param"),
                        QStringLiteral("bleed_mm=3"),
                        QStringLiteral("--param"),
                        QStringLiteral("mode=mirror"),
                        QStringLiteral("--param"),
                        QStringLiteral("force=true"),
                        QStringLiteral("--profile"),
                        preflightSourceDir() + QStringLiteral("/profiles/loop-default.json"),
                        QStringLiteral("--output"),
                        repairedPath,
                        QStringLiteral("--console-format"),
                        QStringLiteral("json") });
        QByteArray stdOut;
        QByteArray stdErr;
        QVERIFY2(test_support::waitForFinishedAndCapture(process, 60000, stdOut, stdErr),
                 qPrintable(QString::fromUtf8(stdErr)));
        QCOMPARE(process.exitCode(), 0);
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(repairedPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    const QVariantList points = host.fixRollbackPoints();
    QVERIFY(!points.isEmpty());
    QVERIFY(host.fixRollbackAvailable());
    QVERIFY(!host.fixRollbackSummary().trimmed().isEmpty());

    for (const QVariant& point : points)
    {
        QVERIFY(!point.toMap().value(QStringLiteral("rollbackId")).toString().isEmpty());
        QVERIFY(!point.toMap().value(QStringLiteral("documentRevisionDigest")).toString().isEmpty());
    }
    const auto acceptedPoint = std::find_if(points.cbegin(), points.cend(), [](const QVariant& point)
                                            { return !point.toMap().value(QStringLiteral("isOriginalInput")).toBool(); });
    QVERIFY(acceptedPoint != points.cend());
    const QString rollbackId = acceptedPoint->toMap().value(QStringLiteral("rollbackId")).toString();

    // An unknown revision is refused before anything is written, and the refusal writes no
    // revision. (A rollback without a validated profile is refused the same way; Core's
    // rollbackRefusesWithoutProfileOrOnCompromisedChain pins that fail-closed path.)
    QVERIFY(!host.requestFixRollback(QStringLiteral("revision-that-was-never-recorded")));
    QVERIFY2(QDir(directory.path()).entryList(QStringList{ QStringLiteral("*-rollback-*.pdf") }, QDir::Files).isEmpty(),
             "a refused rollback must not write a revision");

    // The bundled loop-default profile is the one profile both surfaces can be given, so pin
    // it deterministically: the request supplies the effective profile the restored revision
    // is revalidated under.
    const QString bundledProfileId = QStringLiteral(":/profiles/loop-default.json");
    if (host.selectedPreflightProfileId() != bundledProfileId)
    {
        QVERIFY2(host.selectPreflightProfile(bundledProfileId),
                 qPrintable(QStringLiteral("the bundled loop-default profile must be selectable; selected '%1'")
                                .arg(host.selectedPreflightProfileId())));
    }
    QCOMPARE(host.selectedPreflightProfileId(), bundledProfileId);

    // Core records the rollback as a new event; the scheduled job's completion is the signal
    // to read it. Reading the history while the worker holds its SQLite connection would
    // contend for the database, so wait on the scheduler, not on a probe of the store.
    pdf::PDFJobScheduler& scheduler = host.sessionForTest()->scheduler();
    bool rollbackFinished = false;
    QObject::connect(&scheduler, &pdf::PDFJobScheduler::jobFinished, &host,
                     [&rollbackFinished](const pdf::PDFJobSnapshot& snapshot)
                     {
                         if (snapshot.operationId.startsWith(QStringLiteral("rollback.")))
                         {
                             rollbackFinished = true;
                         }
                     });

    // The request now SCHEDULES the governed restore: it returns true immediately while the
    // revalidation (which refuses to run on this interactive thread) runs on a worker.
    QVERIFY(host.requestFixRollback(rollbackId));
    QTRY_VERIFY_WITH_TIMEOUT(rollbackFinished, 60000);

    // The rolled-back revision is a new sibling file; the open document is never overwritten.
    const QStringList siblings =
        QDir(directory.path()).entryList(QStringList{ QStringLiteral("*-rollback-*.pdf") }, QDir::Files);
    QVERIFY2(!siblings.isEmpty(), "the rollback must write a new revision beside the document");
    QVERIFY(QFile::exists(sourcePath));

    // Core recorded the rollback as a new event and left the existing history intact.
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument() && host.sessionForTest()->facade().source().path != repairedPath, 15000);
    const QString historyPath = host.sessionForTest()->facade().source().path + QStringLiteral(".loop-history/history.sqlite3");
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                        { return event.status == pdf::PDFOperationHistoryStatus::RolledBack; }));
    host.openFileUrl(QUrl::fromLocalFile(repairedPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    rollbackFinished = false;
    QVERIFY(host.requestFixRollback(rollbackId));
    host.openFileUrl(QUrl::fromLocalFile(sourcePath));
    QTRY_VERIFY_WITH_TIMEOUT(rollbackFinished, 60000);
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.sessionForTest()->facade().source().path, sourcePath);
}

// ---------------------------------------------------------------------------
// #17: scheduled results are fenced by request identity
// ---------------------------------------------------------------------------
//
// The fence lives in EditorHost::finishPreflightJob and finishActionListJob, which run
// on the scheduler's jobFinished signal. Each test first lets one run complete for real,
// captures that completion, and then hands the shell the same snapshot for a live request
// with one identity field corrupted. The capture is a plain lambda connection because
// PDFJobSnapshot is not a registered metatype.

void EditorHostTest::preflightFencesCompletionsThatLostTheirRequestIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preflight-fence.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    auto* controller = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    QVERIFY2(controller, "the preflight property must expose the controller that holds the request identity");
    pdf::PDFJobScheduler& scheduler = host.sessionForTest()->scheduler();

    pdf::PDFJobSnapshot realCompletion;
    QString watchedJobId;
    QObject::connect(&scheduler, &pdf::PDFJobScheduler::jobFinished, &host,
                     [&realCompletion, &watchedJobId](const pdf::PDFJobSnapshot& snapshot)
                     {
                         if (!watchedJobId.isEmpty() && snapshot.jobId == watchedJobId)
                         {
                             realCompletion = snapshot;
                         }
                     });

    // The live run must be admitted. Its completion is also the exact snapshot the fault
    // cases below corrupt, so the only difference between a published verdict and a
    // fenced one is the identity field each case breaks.
    QVERIFY(host.runPreflight());
    watchedJobId = controller->jobId();
    QVERIFY(!watchedJobId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY(host.preflightStateName() != QStringLiteral("error"));
    QVERIFY(host.hasPreflightReport());
    QVERIFY(!realCompletion.jobId.isEmpty());
    QCOMPARE(realCompletion.jobId, watchedJobId);
    QCOMPARE(realCompletion.status, pdf::PDFJobStatus::Succeeded);

    for (int fault = 0; fault < 4; ++fault)
    {
        QVERIFY(host.runPreflight());
        watchedJobId = controller->jobId();
        QCOMPARE(controller->state(), pdfinteraction::PreflightController::State::Running);
        QVERIFY(!watchedJobId.isEmpty());

        pdf::PDFJobSnapshot corrupted = realCompletion;
        corrupted.jobId = watchedJobId;
        corrupted.documentKey = controller->documentKey();
        corrupted.documentRevision = controller->documentRevision();
        switch (fault)
        {
            case 0:
                // A completion for a revision the document has already left.
                corrupted.documentRevision = controller->documentRevision() + QStringLiteral("-superseded");
                break;
            case 1:
                // A completion for another document that shares this shell.
                corrupted.documentKey = controller->documentKey() + QStringLiteral("-other-document");
                break;
            case 2:
                // A completion for a different operation on the same revision.
                corrupted.operationId = QStringLiteral("preflight.a-profile-that-was-never-selected");
                break;
            default:
                // A completion that is not the scheduled preflight at all.
                corrupted.kind = pdf::PDFJobKind::Other;
                break;
        }

        scheduler.jobFinished(corrupted);

        // The fenced completion publishes nothing; the earlier verdict stays on screen,
        // marked stale, rather than being replaced by the unowned result.
        QCOMPARE(controller->state(), pdfinteraction::PreflightController::State::Stale);
        QVERIFY(controller->hasResult());
        QVERIFY(host.preflightStateName() != QStringLiteral("pass"));
        QVERIFY(!controller->operatorSummary().trimmed().isEmpty());
    }

    // The fence rejects mismatched completions only: a later live run still completes.
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY(host.preflightStateName() != QStringLiteral("error"));
    QVERIFY(host.hasPreflightReport());
}

void EditorHostTest::actionListFencesCompletionsThatLostTheirRequestIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("fence-job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("fence-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("gh-586-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 9 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    auto* controller = qobject_cast<pdfinteraction::ActionListController*>(host.actionList());
    QVERIFY2(controller, "the actionList property must expose the controller that holds the request identity");
    pdf::PDFJobScheduler& scheduler = host.sessionForTest()->scheduler();

    pdf::PDFJobSnapshot realCompletion;
    QString watchedJobId;
    QObject::connect(&scheduler, &pdf::PDFJobScheduler::jobFinished, &host,
                     [&realCompletion, &watchedJobId](const pdf::PDFJobSnapshot& snapshot)
                     {
                         if (!watchedJobId.isEmpty() && snapshot.jobId == watchedJobId)
                         {
                             realCompletion = snapshot;
                         }
                     });

    // The real plan completion is admitted, and it is the snapshot the faults corrupt.
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 60000);
    QVERIFY(host.planActionList());
    watchedJobId = controller->jobId();
    QVERIFY(!watchedJobId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!realCompletion.jobId.isEmpty());
    QCOMPARE(realCompletion.jobId, watchedJobId);
    QCOMPARE(realCompletion.status, pdf::PDFJobStatus::Succeeded);
    QVERIFY(!host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString().isEmpty());

    for (int fault = 0; fault < 2; ++fault)
    {
        // A stale result clears the validated binding, so every fault needs a fresh
        // validate and plan: the run under test has to be a live request.
        QVERIFY(host.validateActionListRecipe());
        QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 60000);
        QVERIFY(host.planActionList());
        watchedJobId = controller->jobId();
        QCOMPARE(controller->state(), pdfinteraction::ActionListController::State::Planning);

        pdf::PDFJobSnapshot corrupted = realCompletion;
        corrupted.jobId = watchedJobId;
        corrupted.documentKey = controller->documentKey();
        corrupted.documentRevision = controller->documentRevision();
        if (fault == 0)
        {
            // A completion that is not the scheduled Action List work at all.
            corrupted.kind = pdf::PDFJobKind::Preflight;
        }
        else
        {
            // A completion for a different recipe on the same revision.
            corrupted.checkId = QStringLiteral("a-recipe-that-was-never-selected");
        }

        scheduler.jobFinished(corrupted);

        // The controller discards the plan. The shell still names the discarded digest so
        // the surface can show what went stale, so the fence is the lifecycle state.
        QCOMPARE(controller->state(), pdfinteraction::ActionListController::State::Idle);
        QVERIFY(controller->planDigest().isEmpty());
        QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
        QVERIFY(!host.fixExecutionArmed());
        QVERIFY(!controller->operatorSummary().trimmed().isEmpty());
    }

    // The fence rejects mismatched completions only: a later live plan still completes.
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 60000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString().isEmpty());
}

void EditorHostTest::annotationReviewListsAndNavigatesToThePageRegion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("annotated.pdf"));
    QVERIFY(writeAnnotationFixture(documentPath));

    EditorHost host;
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    QuickDocumentModel* documentModel = qobject_cast<QuickDocumentModel*>(host.documentModel());
    QVERIFY(documentModel != nullptr);
    QCOMPARE(documentModel->annotationCount(), 2);

    const QList<QuickAnnotation>& rows = documentModel->annotationModel().rows();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).pageIndex, 0);
    QCOMPARE(rows.at(0).kindName, QStringLiteral("text"));
    QCOMPARE(rows.at(0).title, QStringLiteral("Reviewer"));
    QCOMPARE(rows.at(0).contents, QStringLiteral("Please check the margin"));
    QCOMPARE(rows.at(0).subject, QStringLiteral("Comment"));
    QCOMPARE(rows.at(0).bounds, QRectF(20.0, 40.0, 60.0, 80.0));
    QCOMPARE(rows.at(1).pageIndex, 1);
    QCOMPARE(rows.at(1).title, QStringLiteral("Client"));
    QCOMPARE(rows.at(1).contents, QStringLiteral("Approve page two"));

    // The marks reach the overlay builder as read-only page-space rectangles:
    // no interaction kind, so nothing hit-tests them.
    const QList<pdfinteraction::InteractionTarget>& marks = host.sessionForTest()->overlays()->annotations();
    QCOMPARE(marks.size(), 2);
    QCOMPARE(marks.at(0).pageIndex, 0);
    QCOMPARE(marks.at(0).pageBounds, QRectF(20.0, 40.0, 60.0, 80.0));
    QCOMPARE(marks.at(1).pageIndex, 1);
    QCOMPARE(marks.at(1).pageBounds, QRectF(100.0, 120.0, 60.0, 40.0));
    QCOMPARE(marks.at(0).kind, pdfinteraction::InteractionTargetKind::None);
    QCOMPARE(marks.at(1).kind, pdfinteraction::InteractionTargetKind::None);

    // Navigation reveals the annotation's page region and marks it selected.
    QCOMPARE(host.currentPage(), 0);
    QVERIFY(host.navigateToAnnotation(1));
    QCOMPARE(host.currentPage(), 1);
    QCOMPARE(documentModel->annotationModel().selectedRow(), 1);
    QCOMPARE(host.sessionForTest()->overlays()->selectedAnnotationId(),
             QStringLiteral("annotation:%1").arg(rows.at(1).reference));

    const pdfinteraction::ViewportController& viewport = host.sessionForTest()->viewport();
    const QRectF region = viewport.pagePointToViewportMatrix(1).mapRect(QRectF(100.0, 120.0, 60.0, 40.0));
    QVERIFY(QLineF(region.center(), viewport.viewportRect().center()).length() < 2.0);

    // Out-of-range rows are refused; nothing moves.
    QVERIFY(!host.navigateToAnnotation(2));
    QVERIFY(!host.navigateToAnnotation(-1));
    QCOMPARE(host.currentPage(), 1);
}

void EditorHostTest::annotationCommandsAuthorThroughTheGatewayAndRefuseWithoutGeometry()
{
    qRegisterMetaType<pdfinteraction::CommandResult>("pdfinteraction::CommandResult");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("authoring.pdf"));
    {
        pdf::PDFDocumentBuilder builder;
        builder.appendPage(QRectF(0, 0, 612, 792));
        pdf::PDFDocumentWriter writer(nullptr);
        const pdf::PDFDocument document = builder.build();
        QVERIFY(writer.write(documentPath, &document, false));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QuickDocumentModel* documentModel = qobject_cast<QuickDocumentModel*>(host.documentModel());
    QVERIFY(documentModel != nullptr);
    QCOMPARE(documentModel->annotationCount(), 0);

    // The command is available with an unrestricted document.
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionCreateTextHighlight")));

    QSignalSpy finished(&session->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);

    // A menu invocation carries no geometry: the catalog refuses the missing
    // required parameter before any handler runs, typed, and nothing is created.
    QCOMPARE(host.invokeCommand(QStringLiteral("actionCreateTextHighlight")), quint64(0));
    QCOMPARE(finished.count(), 1);
    {
        const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
        QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Failed);
        QCOMPARE(result.typedError, QStringLiteral("command/missing-parameter"));
    }
    QCOMPARE(documentModel->annotationCount(), 0);

    // A degenerate rectangle passes the contract's type check and is refused by
    // the handler's boundary parse, before the gateway is reached.
    QVERIFY(host.invokeCommand(QStringLiteral("actionCreateTextHighlight"),
                               QVariantMap{ { QStringLiteral("page"), 0 },
                                            { QStringLiteral("x"), 10.0 },
                                            { QStringLiteral("y"), 10.0 },
                                            { QStringLiteral("width"), 0.0 },
                                            { QStringLiteral("height"), 10.0 } }) != 0);
    QCOMPARE(finished.count(), 1);
    {
        const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
        QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Failed);
        QCOMPARE(result.typedError, QStringLiteral("annotation/invalid-geometry"));
    }
    QCOMPARE(documentModel->annotationCount(), 0);

    // With geometry the handler submits the typed request, the gateway creates
    // the annotation as a new revision, and the quick lane presents it.
    const QString beforeRevision = session->facade().currentRevision().toString();
    const QVariantMap parameters{
        { QStringLiteral("page"), 0 },
        { QStringLiteral("x"), 100.0 },
        { QStringLiteral("y"), 120.0 },
        { QStringLiteral("width"), 200.0 },
        { QStringLiteral("height"), 16.0 },
    };
    QVERIFY(host.invokeCommand(QStringLiteral("actionCreateTextHighlight"), parameters) != 0);
    QCOMPARE(finished.count(), 1);
    {
        const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
        QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Completed);
        QVERIFY2(result.typedError.isEmpty(), qPrintable(result.typedError));
    }
    QVERIFY(session->facade().currentRevision().toString() != beforeRevision);

    const pdf::PDFPage* page = session->context().getDocument()->getCatalog()->getPage(0);
    QVERIFY(page != nullptr);
    QCOMPARE(page->getAnnotations().size(), std::size_t(1));
    const pdf::PDFAnnotationPtr annotation =
        pdf::PDFAnnotation::parse(&session->context().getDocument()->getStorage(), page->getAnnotations().front());
    QVERIFY(annotation != nullptr);
    QCOMPARE(annotation->getType(), pdf::AnnotationType::Highlight);
    QCOMPARE(annotation->getRectangle(), QRectF(100.0, 120.0, 200.0, 16.0));

    // The revision change reached the quick lane: the annotation model lists the
    // new mark and the overlay carries it as a read-only page-space rectangle.
    QTRY_COMPARE_WITH_TIMEOUT(documentModel->annotationCount(), 1, 10000);
    const QList<QuickAnnotation>& rows = documentModel->annotationModel().rows();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).pageIndex, 0);
    QCOMPARE(rows.at(0).bounds, QRectF(100.0, 120.0, 200.0, 16.0));
    QCOMPARE(session->overlays()->annotations().size(), 1);

    // The accepted edit appended its provenance event beside the document
    // (#124's model; wiring Undo stays out of this slice).
    QVERIFY(QFile::exists(documentPath + QStringLiteral(".loop-history/history.sqlite3")));
}

void EditorHostTest::signatureStatusPresentsTheReducedState()
{
    const QString fixture = signedFixturePath();
    QVERIFY2(QFile::exists(fixture), qPrintable(fixture));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // 1. The committed fixture: its certificate is self-signed, so trust cannot
    //    be established. That is `unknown`, never a pass.
    {
        EditorHost host;
        host.openFileUrl(QUrl::fromLocalFile(fixture));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(host.signatureStateName(), QStringLiteral("unknown"), 30000);
        QCOMPARE(host.signatureStateVisual().value(QStringLiteral("kind")).toString(), QStringLiteral("Incomplete"));
        QVERIFY(host.signatureSummary().contains(QStringLiteral("could not be verified")));
    }

    // 2. One byte changed inside the signed range: the digest no longer matches.
    const QString tamperedPath = directory.filePath(QStringLiteral("tampered.pdf"));
    QVERIFY(writeSignatureTamperedCopy(fixture, tamperedPath));
    {
        EditorHost host;
        host.openFileUrl(QUrl::fromLocalFile(tamperedPath));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(host.signatureStateName(), QStringLiteral("invalid"), 30000);
        QCOMPARE(host.signatureStateVisual().value(QStringLiteral("kind")).toString(), QStringLiteral("Error"));
    }

    // 3. Bytes appended after the signature: the signature still verifies but no
    //    longer covers the whole file.
    const QString appendedPath = directory.filePath(QStringLiteral("appended.pdf"));
    QVERIFY(writeSignatureAppendedCopy(fixture, appendedPath));
    {
        EditorHost host;
        host.openFileUrl(QUrl::fromLocalFile(appendedPath));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(host.signatureStateName(), QStringLiteral("modified-after-signing"), 30000);
        QCOMPARE(host.signatureStateVisual().value(QStringLiteral("kind")).toString(), QStringLiteral("Warning"));
    }

    // 4. An unsigned document: the check completes and says exactly that.
    const QString unsignedPath = directory.filePath(QStringLiteral("unsigned.pdf"));
    QVERIFY(writeTrimmedPageDocument(unsignedPath));
    {
        EditorHost host;
        host.openFileUrl(QUrl::fromLocalFile(unsignedPath));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(host.signatureStateName(), QStringLiteral("not-signed"), 30000);
    }
}

void EditorHostTest::signatureInvalidationGateBlocksExecutionUntilAcknowledged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("signed-trimmed.pdf"));
    QVERIFY(writeSignedTrimmedPageDocument(documentPath));

    const QString recipePath = directory.filePath(QStringLiteral("gh-125-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("gh-125-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 3 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // The signature check must have presented a signed state before the plan is
    // judged, so the disclosure is not skipped by a race with the check job.
    QTRY_VERIFY_WITH_TIMEOUT(host.signatureStateName() != QStringLiteral("not-signed"), 30000);

    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    const QVariantMap planIdentity = host.fixPlanIdentity();
    const QString planDigest = planIdentity.value(QStringLiteral("planDigest")).toString();
    QVERIFY(!planDigest.isEmpty());
    QCOMPARE(planIdentity.value(QStringLiteral("signatureInvalidationApplies")).toBool(), true);
    QCOMPARE(planIdentity.value(QStringLiteral("signatureInvalidationAcknowledged")).toBool(), false);

    // The preview discloses the invalidation from the save policy's own field.
    const QVariantMap preview = host.fixPreview();
    const QVariantMap invalidation = preview.value(QStringLiteral("signatureInvalidation")).toMap();
    QCOMPARE(invalidation.value(QStringLiteral("applies")).toBool(), true);
    QVERIFY(!invalidation.value(QStringLiteral("summary")).toString().trimmed().isEmpty());

    // Approval alone never arms execution for an invalidating plan.
    QVERIFY(host.approveActionListPlan(planDigest,
                                       planIdentity.value(QStringLiteral("sourceSha256")).toString(),
                                       planIdentity.value(QStringLiteral("plannedRevision")).toString()));
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(host.actionListStateName(), QStringLiteral("planned"));
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // An acknowledgement for any other digest does not count.
    QVERIFY(!host.acknowledgePlanSignatureInvalidation(QString(64, QLatin1Char('0'))));
    QVERIFY(!host.fixPlanIdentity().value(QStringLiteral("signatureInvalidationAcknowledged")).toBool());
    QVERIFY(!host.fixExecutionArmed());

    // The acknowledgement is the whole difference: armed, and the same plan
    // executes to a published candidate.
    QVERIFY(host.acknowledgePlanSignatureInvalidation(planDigest));
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("signatureInvalidationAcknowledged")).toBool(), true);
    QVERIFY(host.fixExecutionArmed());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("succeeded"), 60000);
}

void EditorHostTest::previewFidelityNamesTheOriginAndSwitchesExplicitly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preview-fidelity.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("none"));
    QVERIFY(host.previewFidelitySummary().contains(QStringLiteral("No rendered evidence")));
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);

    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("fast-canvas"));
    QVERIFY(!host.previewRequiresAuthoritative());
    QVERIFY(!host.ensureAuthoritativePreview());
    QVERIFY(!host.previewFidelitySummary().trimmed().isEmpty());

    host.toggleCurrentPageFidelity();
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("none"));
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("authoritative"), 30000);
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("output-preview"));
    QVERIFY(!host.previewRequiresAuthoritative());

    const QString overprintPath =
        preflightFixturesDir() + QStringLiteral("/overprint-cmyk-mode1-on.pdf");
    QVERIFY2(QFileInfo::exists(overprintPath), qPrintable(overprintPath));

    EditorHost overprintHost(nullptr, nullptr, m_fixtureRecipes->path());
    overprintHost.openFileUrl(QUrl::fromLocalFile(overprintPath));
    QTRY_VERIFY_WITH_TIMEOUT(overprintHost.hasDocument(), 30000);
    overprintHost.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_VERIFY_WITH_TIMEOUT(overprintHost.previewFidelityStateName() == QStringLiteral("approximate"), 30000);
    QCOMPARE(overprintHost.previewFidelityOriginName(), QStringLiteral("fast-canvas"));
    QVERIFY(overprintHost.previewRequiresAuthoritative());
    QVERIFY(overprintHost.previewFidelitySummary().contains(QStringLiteral("print-safe output")));

    const QVariantMap approximate = overprintHost.previewFidelityVisual();
    QVERIFY(approximate.value(QStringLiteral("kind")).toString() != QStringLiteral("Passed"));
    QVERIFY(approximate.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));

    QVERIFY(overprintHost.ensureAuthoritativePreview());
    QCOMPARE(overprintHost.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(overprintHost.previewFidelityOriginName(), QStringLiteral("none"));
    QVERIFY(!overprintHost.ensureAuthoritativePreview());
    QTRY_COMPARE_WITH_TIMEOUT(overprintHost.previewFidelityStateName(), QStringLiteral("authoritative"), 30000);
    QCOMPARE(overprintHost.previewFidelityOriginName(), QStringLiteral("output-preview"));
    QVERIFY(!overprintHost.previewRequiresAuthoritative());
    QVERIFY(!overprintHost.ensureAuthoritativePreview());
}

void EditorHostTest::refusedRenderCannotBecomeAuthoritativeEvidence()
{
    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(preflightFixturesDir() + QStringLiteral("/overprint-cmyk-mode1-on.pdf")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    auto* surfaces = host.sessionForTest()->surfaces();
    pdf::PDFResourceBudgetConfig budget;
    budget.setLimit(pdf::PDFResourcePool::RasterTileCache, 1024);
    surfaces->setResourceBudget(std::make_shared<pdf::PDFResourceBudget>(budget));
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_VERIFY_WITH_TIMEOUT(surfaces->counters().budgetExhausted > 0 || surfaces->counters().rejectedOversize > 0, 30000);
    QVERIFY(!surfaces->diagnosticsForPage(0).has_value());
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));

    host.toggleCurrentPageFidelity();
    QVERIFY(host.pageFidelityIsAuthoritative());
    QVERIFY(!surfaces->diagnosticsForPage(0).has_value());
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("none"));
    QVERIFY(host.previewFidelitySummary().contains(QStringLiteral("No rendered evidence")));
    QVERIFY(!host.ensureAuthoritativePreview());
}

void EditorHostTest::executeApprovedActionListPlanRefusesAnUnreviewedPlan()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("unreviewed.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("unreviewed-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("unreviewed-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 9 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(host.actionListStateName(), QStringLiteral("planned"));
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.fixExecutionArmed());
    QVERIFY(host.executeApprovedActionListPlan());
}

void EditorHostTest::outputBytesAndFences()
{
    QTemporaryDir directory;
    const QString source = directory.filePath(QStringLiteral("source.pdf"));
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 72, 144));
    const auto document = builder.build();
    QVERIFY(pdf::PDFDocumentWriter(nullptr).write(source, &document, true));
    QFile input(source);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto bytes = input.readAll();
    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
    const QString destination = directory.filePath(QStringLiteral("export.pdf"));
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(destination), QStringLiteral("pdf"), 300, QStringLiteral("stale"), true));
    QVERIFY(!QFile::exists(destination));
    QCOMPARE(host.outputRecord().value(QStringLiteral("error")).toString(), QStringLiteral("output/stale-or-busy-document"));
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(destination), QStringLiteral("pdf"), 300, host.outputRevision(), true));
    QTRY_VERIFY_WITH_TIMEOUT(!host.outputBusy(), 10000);
    QCOMPARE(host.outputRecord().value(QStringLiteral("status")).toString(), QStringLiteral("saved"));
    QFile exported(destination);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), bytes);
    exported.close();
    QCOMPARE(host.outputRecord().value(QStringLiteral("artifact")).toMap().value(QStringLiteral("sha256")).toString(),
             QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
    QVERIFY(!host.outputRecord().value(QStringLiteral("printProof")).toBool());
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(source), QStringLiteral("pdf"), 300, host.outputRevision(), true));
    QCOMPARE(host.outputRecord().value(QStringLiteral("error")).toString(), QStringLiteral("output/invalid-or-source-destination"));

    const QVariantMap parameters{ { QStringLiteral("path"), destination }, { QStringLiteral("format"), QStringLiteral("print-pdf") }, { QStringLiteral("dpi"), 72 }, { QStringLiteral("revision"), host.outputRevision() }, { QStringLiteral("fidelityAcknowledged"), true } };
    const auto invocation = host.invokeCommand(QStringLiteral("actionPrint"), parameters);
    QVERIFY(invocation != pdfinteraction::InvalidCommandInvocation);
    QVERIFY(host.cancelCommand(invocation));
    QTRY_VERIFY_WITH_TIMEOUT(!host.outputBusy(), 10000);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), bytes);
    exported.close();
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(destination), QStringLiteral("print-pdf"), 72, host.outputRevision(), true));
    host.invokeCommand(QStringLiteral("actionClose"));
    QTRY_VERIFY_WITH_TIMEOUT(!host.outputBusy(), 10000);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), bytes);
}

void EditorHostTest::outputPublishedPacket()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 72, 144));
    loopeditor::OutputRequest request;
    request.document = pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build()));
    request.revision = QStringLiteral("accepted-revision");
    request.publishedBytes = QByteArray("%PDF-1.7\nexact published packet\n%%EOF");
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(request.publishedBytes, QCryptographicHash::Sha256).toHex());
    request.publicationReceipt = { { QStringLiteral("status"), QStringLiteral("published") },
                                   { QStringLiteral("published_sha256"), digest },
                                   { QStringLiteral("plan_digest"), QStringLiteral("reviewed-plan") } };
    pdf::PDFJobContext job(std::make_shared<pdf::PDFJobCancellationToken>(), pdf::PDFProcessingLimits::conservativeDefaults(), {});
    loopeditor::OutputResult result;
    loopeditor::prepareOutput(request, result, job);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QFile exported(result.stagedPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), request.publishedBytes);
    QCOMPARE(result.record.value(QStringLiteral("publicationReceipt")).toObject(), request.publicationReceipt);
    request.publicationReceipt.insert(QStringLiteral("published_sha256"), QString(64, QLatin1Char('0')));
    loopeditor::OutputResult refused;
    loopeditor::prepareOutput(request, refused, job);
    QCOMPARE(refused.error, QStringLiteral("output/publication-identity-mismatch"));
    QVERIFY(!QFile::exists(refused.stagedPath));
}

void EditorHostTest::outputRenderFormats_data()
{
    QTest::addColumn<QString>("format");
    QTest::newRow("print-to-PDF") << QStringLiteral("print-pdf");
    QTest::newRow("PNG") << QStringLiteral("png");
    QTest::newRow("TIFF") << QStringLiteral("tiff");
}

void EditorHostTest::outputRenderFormats()
{
    QFETCH(QString, format);
    if (format == QLatin1String("tiff") && !QImageWriter::supportedImageFormats().contains(QByteArray("tiff")))
        QSKIP("Qt TIFF encoder is unavailable; the product refuses TIFF output explicitly.");
    QTemporaryDir directory;
    const QString source = directory.filePath(QStringLiteral("source.pdf"));
    pdf::PDFDocumentBuilder builder;
    const auto firstPage = builder.appendPage(QRectF(0, 0, 72, 144));
    builder.appendPage(QRectF(0, 0, 144, 72));
    pdf::PDFPageContentStreamBuilder contents(&builder);
    auto* painter = contents.begin(firstPage);
    QVERIFY(painter);
    painter->fillRect(QRectF(0, 0, 36, 144), Qt::black);
    contents.end(painter);
    const auto document = builder.build();
    QVERIFY(pdf::PDFDocumentWriter(nullptr).write(source, &document, true));
    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
    const QString destination = directory.filePath(QStringLiteral("output"));
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    host.goToPage(0);
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(destination), format, 72, host.outputRevision(), true));
    QTRY_VERIFY_WITH_TIMEOUT(!host.outputBusy(), 20000);
    const auto record = host.outputRecord();
    QVERIFY2(record.value(QStringLiteral("status")).toString() == QLatin1String("saved"), qPrintable(record.value(QStringLiteral("error")).toString()));
    QCOMPARE(record.value(QStringLiteral("dpi")).toInt(), 72);
    QCOMPARE(record.value(QStringLiteral("renderer")).toString(), QStringLiteral("Loop Core PDFTransparencyRenderer"));
    QVERIFY(!record.value(QStringLiteral("colorHandling")).toString().isEmpty());
    QVERIFY(!record.value(QStringLiteral("printProof")).toBool());
    if (format == QLatin1String("print-pdf"))
    {
        pdf::PDFDocumentReader reader(nullptr, {}, true, false);
        const auto printed = reader.readFromFile(destination);
        QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);
        QCOMPARE(printed.getCatalog()->getPageCount(), size_t(2));
        QCOMPARE(printed.getCatalog()->getPage(0)->getMediaBox().size(), QSizeF(72, 144));
        QCOMPARE(printed.getCatalog()->getPage(1)->getMediaBox().size(), QSizeF(144, 72));
        loopeditor::OutputRequest request;
        request.format = loopeditor::OutputFormat::Png;
        request.document = pdf::PDFDocumentPointer(new pdf::PDFDocument(printed));
        request.revision = QStringLiteral("printed-fixture");
        request.dpi = 72;
        request.fidelityAcknowledged = true;
        loopeditor::OutputResult result;
        pdf::PDFJobContext job(std::make_shared<pdf::PDFJobCancellationToken>(), pdf::PDFProcessingLimits::conservativeDefaults(), {});
        loopeditor::prepareOutput(request, result, job);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        const auto image = QImageReader(result.stagedPath).read();
        QCOMPARE(image.pixelColor(18, 72), QColor(Qt::black));
        QCOMPARE(image.pixelColor(54, 72), QColor(Qt::white));
    }
    else
    {
        QImageReader reader(destination);
        const auto image = reader.read();
        QCOMPARE(image.size(), QSize(72, 144));
        QCOMPARE(image.pixelColor(18, 72), QColor(Qt::black));
        QCOMPARE(image.pixelColor(54, 72), QColor(Qt::white));
        QVERIFY(qAbs(image.dotsPerMeterX() - qRound(72 / 0.0254)) <= 1);
    }
    QFile artifact(destination);
    QVERIFY(artifact.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(artifact.readAll(), QCryptographicHash::Sha256).toHex()),
             record.value(QStringLiteral("artifact")).toMap().value(QStringLiteral("sha256")).toString());
    qInfo().noquote() << "quick-output-fixture" << format << "source_sha256="
                      << QString::fromLatin1(host.sessionForTest()->context().getDocumentPointer()->getSourceDataHash().toHex())
                      << "artifact_sha256=" << record.value(QStringLiteral("artifact")).toMap().value(QStringLiteral("sha256")).toString();
}

void EditorHostTest::outputOverprintWarning()
{
    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(preflightFixturesDir() + QStringLiteral("/overprint-cmyk-mode0-on.pdf")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 10000);
    host.goToPage(0);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_VERIFY_WITH_TIMEOUT(!host.pageFidelityIsExact(), 10000);
    QTemporaryDir directory;
    const QString destination = directory.filePath(QStringLiteral("output.png"));
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(destination), QStringLiteral("png"), 72, host.outputRevision(), false));
    QCOMPARE(host.outputRecord().value(QStringLiteral("error")).toString(), QStringLiteral("output/fidelity-acknowledgment-required"));
    QVERIFY(!QFile::exists(destination));
    QVERIFY(host.exportDocumentFileUrl(QUrl::fromLocalFile(destination), QStringLiteral("png"), 72, host.outputRevision(), true));
    QTRY_VERIFY_WITH_TIMEOUT(!host.outputBusy(), 20000);
    QCOMPARE(host.outputRecord().value(QStringLiteral("status")).toString(), QStringLiteral("saved"));
    QVERIFY(host.outputRecord().value(QStringLiteral("fidelityAcknowledged")).toBool());
    QVERIFY(!host.outputRecord().value(QStringLiteral("printProof")).toBool());
}

void EditorHostTest::outputRenderFailure()
{
    pdf::PDFDocumentBuilder builder;
    const auto page = builder.appendPage(QRectF(0, 0, 72, 144));
    const auto stream = builder.addObject(pdf::PDFObject::createStream(
        std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray("unknownOperator"))));
    pdf::PDFDictionary dictionary;
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(stream));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(dictionary))));
    loopeditor::OutputRequest request;
    request.document = pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build()));
    request.format = loopeditor::OutputFormat::Png;
    request.revision = QStringLiteral("broken-content-fixture");
    request.dpi = 72;
    request.fidelityAcknowledged = true;
    loopeditor::OutputResult result;
    pdf::PDFJobContext job(std::make_shared<pdf::PDFJobCancellationToken>(), pdf::PDFProcessingLimits::conservativeDefaults(), {});
    loopeditor::prepareOutput(request, result, job);
    QVERIFY2(result.error.startsWith(QLatin1String("output/render-failed")), qPrintable(result.error));
    QVERIFY(!QFile::exists(result.stagedPath));
    QVERIFY(!result.record.contains(QStringLiteral("artifact")));
}

/// Issue #235, display half. A display colour mode changes what the canvas shows and
/// nothing else: it is marked a simulation by the fidelity indicator, and neither the
/// Compare workspace nor the production identity can see it. The failure case is a
/// display mode mistaken for a production render.
void EditorHostTest::displayColorModesChangePresentationAndAreNeverProductionEvidence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("display-modes.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);

    // Every display mode is a bound command with a document open.
    const QStringList actions = { QStringLiteral("actionColorInvert"), QStringLiteral("actionColorGrayscale"),
                                  QStringLiteral("actionColorHighContrast"), QStringLiteral("actionColorBitonal"),
                                  QStringLiteral("actionColorCustom") };
    for (const QString& action : actions)
    {
        QVERIFY2(host.isCommandEnabled(action), qPrintable(action));
    }

    const QVariantMap compareBefore = host.compareReview();
    const QVariantMap identityBefore = host.previewIdentity();
    const QVariantMap signOffBefore = host.fixSignOff();
    const QString revisionBefore = host.sessionForTest()->facade().currentRevision().toString();

    // The menu entry works, and what it produces is a simulation -- not the render
    // path's fidelity, and never a pass.
    QVERIFY(host.invokeCommand(QStringLiteral("actionColorInvert")) != 0);
    QCOMPARE(host.displayColorModeName(), QStringLiteral("invert"));
    QVERIFY(host.displayColorModeActive());
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("display-filter"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("display-filter"));
    QVERIFY(host.previewFidelitySummary().contains(QStringLiteral("never recorded as approval evidence")));

    const QVariantMap visual = host.previewFidelityVisual();
    QCOMPARE(visual.value(QStringLiteral("kind")).toString(), QStringLiteral("Warning"));
    QVERIFY(visual.value(QStringLiteral("kind")).toString() != QStringLiteral("Passed"));
    QVERIFY(visual.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));
    QVERIFY(!visual.value(QStringLiteral("accessibleName")).toString().trimmed().isEmpty());

    // The failure case: nothing in Compare or sign-off may mistake it for a production
    // render. The comparison, the preview identity and the sign-off are the same facts
    // they were before the mode existed.
    QCOMPARE(host.compareReview(), compareBefore);
    QCOMPARE(host.previewIdentity(), identityBefore);
    QCOMPARE(host.fixSignOff(), signOffBefore);
    QVERIFY(!host.fixPreview().value(QStringLiteral("available")).toBool());
    QCOMPARE(host.sessionForTest()->facade().currentRevision().toString(), revisionBefore);

    // Invoking the mode already in force returns to normal: there is no sixth action,
    // and a display mode with no way back is a trap.
    QVERIFY(host.invokeCommand(QStringLiteral("actionColorInvert")) != 0);
    QCOMPARE(host.displayColorModeName(), QStringLiteral("normal"));
    QVERIFY(!host.displayColorModeActive());
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);

    // Each of the remaining modes is reachable and distinct.
    for (const QString& action : { QStringLiteral("actionColorGrayscale"), QStringLiteral("actionColorHighContrast"),
                                   QStringLiteral("actionColorBitonal"), QStringLiteral("actionColorCustom") })
    {
        QVERIFY(host.invokeCommand(action) != 0);
        QVERIFY(host.displayColorModeActive());
        QCOMPARE(host.previewFidelityStateName(), QStringLiteral("display-filter"));
    }
    QCOMPARE(host.displayColorModeName(), QStringLiteral("custom"));
    QVERIFY(host.invokeCommand(QStringLiteral("actionColorCustom")) != 0);
    QCOMPARE(host.displayColorModeName(), QStringLiteral("normal"));
}

void EditorHostTest::displayColorModeNamesAreParsedOnceAtTheBoundary()
{
    EditorHost host;

    QVERIFY(!host.setDisplayColorMode(QStringLiteral("sepia")));
    QCOMPARE(host.displayColorModeName(), QStringLiteral("normal"));
    QVERIFY(!host.setDisplayColorMode(QString()));
    QCOMPARE(host.displayColorModeName(), QStringLiteral("normal"));

    QVERIFY(host.setDisplayColorMode(QStringLiteral("Grayscale")));
    QCOMPARE(host.displayColorModeName(), QStringLiteral("grayscale"));
    QVERIFY(host.setDisplayColorMode(QStringLiteral("normal")));

    // The custom mode's settings are validated at the boundary too: an invalid colour
    // or an out-of-range threshold is refused rather than coerced.
    QVERIFY(host.setDisplayColorCustomColors(QColor(10, 20, 30), QColor(200, 210, 220)));
    QCOMPARE(host.displayColorCustomForeground(), QColor(10, 20, 30));
    QCOMPARE(host.displayColorCustomBackground(), QColor(200, 210, 220));
    QVERIFY(!host.setDisplayColorCustomColors(QColor(), QColor(200, 210, 220)));
    QVERIFY(!host.setDisplayColorCustomColors(QColor(10, 20, 30), QColor()));

    QVERIFY(host.setDisplayColorBitonalThreshold(200));
    QCOMPARE(host.displayColorBitonalThreshold(), 200);
    QVERIFY(!host.setDisplayColorBitonalThreshold(0));
    QVERIFY(!host.setDisplayColorBitonalThreshold(255));
    QCOMPARE(host.displayColorBitonalThreshold(), 200);
}

/// Issue #237. The soft proof renders through Core's colour management under proofing
/// settings, shows the profile identity, and is marked a simulation. It is never
/// recorded as approval evidence, and it never replaces the document session's own CMS.
void EditorHostTest::softProofSimulatesThroughCoreColourManagementAndNeverBecomesEvidence()
{
    const QString path = preflightFixturesDir() + QStringLiteral("/output-intent-cmyk.pdf");
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    // A page's fidelity is only reported once the shell has a layout and the
    // page's surface has been requested.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), 0, 30000);

    QCOMPARE(host.softProofStateName(), QStringLiteral("off"));
    QVERIFY(!host.softProofActive());
    QVERIFY(!host.softProofSummary().trimmed().isEmpty());

    QVERIFY(host.setSoftProofEnabled(true));
    QCOMPARE(host.softProofStateName(), QStringLiteral("simulating"));
    QVERIFY(host.softProofActive());
    QVERIFY(!host.softProofProfileId().isEmpty());
    QCOMPARE(host.softProofProfileTypeName(), QStringLiteral("output-intent"));
    // The identity shown is the profile's: its Core-issued identifier always, and
    // its name when the profile carries one.
    QVERIFY(host.softProofSummary().contains(host.softProofProfileId()));
    QVERIFY(host.softProofSummary().contains(host.softProofProfileName().isEmpty()
                                                 ? host.softProofProfileId()
                                                 : host.softProofProfileName()));

    // The proof is its own render identity: the surfaces re-render as proofs, through
    // Core's colour management, and the diagnostics carry the simulation's reason.
    // Until that render is admitted the indicator honestly says `unavailable`, so the
    // simulation state is asserted once the proof is on screen.
    auto* surfaces = host.sessionForTest()->surfaces();
    QVERIFY(surfaces->isSoftProofing());
    QCOMPARE(surfaces->softProofProfile(), host.softProofProfileId());
    QTRY_VERIFY_WITH_TIMEOUT(surfaces->diagnosticsForPage(0).has_value() && surfaces->diagnosticsForPage(0)->reasons.contains(QStringLiteral("soft-proof/simulation")),
                             30000);
    QVERIFY(!host.pageFidelityIsExact());

    // The indicator says simulation, and the origin names the proofing path rather
    // than the fast canvas or the output-preview render.
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("soft-proof"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("soft-proof"));
    const QVariantMap visual = host.previewFidelityVisual();
    QVERIFY(visual.value(QStringLiteral("kind")).toString() != QStringLiteral("Passed"));
    QVERIFY(visual.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));

    // Never approval evidence: no correction preview, no sign-off, no comparison.
    QVERIFY(!host.fixPreview().value(QStringLiteral("available")).toBool());
    QVERIFY(host.fixSignOff().value(QStringLiteral("planDigest")).toString().isEmpty());

    // Turning it off restores the unproofed identity.
    QVERIFY(!host.toggleSoftProof());
    QCOMPARE(host.softProofStateName(), QStringLiteral("off"));
    QVERIFY(!host.softProofActive());
    QVERIFY(!surfaces->isSoftProofing());
    QVERIFY(host.softProofProfileId().isEmpty());
    QVERIFY(!host.previewFidelityStateName().contains(QStringLiteral("soft-proof")));
}

void EditorHostTest::softProofWithoutAUsableOutputIntentIsAVisibleState()
{
    // A document that names no output intent at all.
    {
        const QString path = preflightFixturesDir() + QStringLiteral("/output-intent-missing.pdf");
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));

        EditorHost host;
        host.openFileUrl(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
        host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
        QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);

        QVERIFY(!host.setSoftProofEnabled(true));
        QCOMPARE(host.softProofStateName(), QStringLiteral("profile-missing"));
        QVERIFY(!host.softProofActive());
        QVERIFY(host.softProofProfileId().isEmpty());
        QVERIFY(host.softProofSummary().contains(QStringLiteral("no output-intent profile")));

        // Never a silent sRGB fallback: the surfaces keep the unproofed identity, and
        // the indicator still describes the render path, not a simulation.
        QVERIFY(!host.sessionForTest()->surfaces()->isSoftProofing());
        QVERIFY(host.previewFidelityStateName() != QStringLiteral("soft-proof"));
        QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("fast-canvas"));
    }

    // A document that names one Core cannot read.
    {
        const QString path = preflightFixturesDir() + QStringLiteral("/output-intent-profile-invalid.pdf");
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));

        EditorHost host;
        host.openFileUrl(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

        QVERIFY(!host.setSoftProofEnabled(true));
        QCOMPARE(host.softProofStateName(), QStringLiteral("profile-invalid"));
        QVERIFY(!host.softProofActive());
        QVERIFY(host.softProofSummary().contains(QStringLiteral("could not be read")));
        QVERIFY(!host.sessionForTest()->surfaces()->isSoftProofing());
    }
}

/// Issue #237. The plates are Core's own separations for the document, and the
/// operator's visibility and isolation over them is presentation state: the document
/// revision does not move.
void EditorHostTest::platesListSeparationsAndCarryVisibilityWithoutTouchingTheDocument()
{
    const QString path = preflightFixturesDir() + QStringLiteral("/color-inventory-spot.pdf");
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    const QVariantList plates = host.productionPlates();
    QVERIFY(plates.size() >= 5);

    QSet<QString> ids;
    bool sawProcess = false;
    bool sawSpot = false;
    for (const QVariant& entry : plates)
    {
        const QVariantMap row = entry.toMap();
        const QString id = row.value(QStringLiteral("id")).toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(!row.value(QStringLiteral("displayName")).toString().isEmpty());
        QVERIFY(row.value(QStringLiteral("visible")).toBool());
        ids.insert(id);
        if (row.value(QStringLiteral("kind")).toString() == QStringLiteral("spot"))
        {
            sawSpot = true;
            QCOMPARE(row.value(QStringLiteral("spotColorName")).toString(), id);
        }
        else
        {
            sawProcess = true;
        }
    }
    QVERIFY(sawProcess);
    QVERIFY(sawSpot);
    QVERIFY(ids.contains(QStringLiteral("Cyan")));
    QVERIFY(ids.contains(QStringLiteral("Black")));
    QVERIFY(ids.contains(QStringLiteral("PANTONE 185 C")));
    QVERIFY(host.plateIsolationSummary().contains(QStringLiteral("Every plate is shown")));

    const QString revisionBefore = host.sessionForTest()->facade().currentRevision().toString();
    const QString spotId = QStringLiteral("PANTONE 185 C");

    const auto rowFor = [&host](const QString& id)
    {
        for (const QVariant& entry : host.productionPlates())
        {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("id")).toString() == id)
            {
                return row;
            }
        }
        return QVariantMap();
    };

    // Hiding a plate is presentation state, and it is visible in the summary.
    QVERIFY(host.setPlateVisible(spotId, false));
    QVERIFY(!rowFor(spotId).value(QStringLiteral("visible")).toBool());
    QVERIFY(host.plateIsolationSummary().contains(QStringLiteral("Hidden plates")));
    QVERIFY(host.plateIsolationSummary().contains(spotId));

    // Isolating one shows exactly one.
    QVERIFY(host.isolatePlate(QStringLiteral("Cyan")));
    for (const QVariant& entry : host.productionPlates())
    {
        const QVariantMap row = entry.toMap();
        QCOMPARE(row.value(QStringLiteral("visible")).toBool(),
                 row.value(QStringLiteral("id")).toString() == QStringLiteral("Cyan"));
        QCOMPARE(row.value(QStringLiteral("isolated")).toBool(),
                 row.value(QStringLiteral("id")).toString() == QStringLiteral("Cyan"));
    }
    QVERIFY(host.plateIsolationSummary().contains(QStringLiteral("Isolated plate: Cyan")));

    QVERIFY(host.showAllPlates());
    QVERIFY(host.plateIsolationSummary().contains(QStringLiteral("Every plate is shown")));

    // An unknown plate id is refused rather than inventing a plate.
    QVERIFY(!host.setPlateVisible(QStringLiteral("No Such Ink"), false));
    QVERIFY(!host.isolatePlate(QStringLiteral("No Such Ink")));

    // Presentation state: the document revision never moved.
    QCOMPARE(host.sessionForTest()->facade().currentRevision().toString(), revisionBefore);
}

/// Issue #237. The readout is measured through Core's separation-accurate path, and a
/// readout that could not be produced is a visible reason -- never a zero, which would
/// read as "no ink here".
void EditorHostTest::inkCoverageReadoutAtAPointIsMeasuredOrExplicitlyUnavailable()
{
    const QString path = preflightFixturesDir() + QStringLiteral("/color-cmyk.pdf");
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    // A point on the current page is only meaningful once the shell has a layout.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), 0, 30000);

    // Nothing has been measured yet.
    QVERIFY(!host.inkCoverageReadout().value(QStringLiteral("available")).toBool());

    // Inside the page: a real measurement with per-plate channels. The channel count
    // is whatever the page's separated device space has -- three for this fixture --
    // so the assertion is on the shape of the answer, not on a number: named plates,
    // coverage in [0, 1] each, and a total that is their sum.
    QVERIFY(host.requestInkCoverageAtPoint(150.0, 150.0));
    QTRY_VERIFY_WITH_TIMEOUT(host.inkCoverageReadout().value(QStringLiteral("available")).toBool(), 30000);
    const QVariantMap readout = host.inkCoverageReadout();
    QCOMPARE(readout.value(QStringLiteral("pageIndex")).toInt(), 0);
    const int channelCount = readout.value(QStringLiteral("channelCount")).toInt();
    QVERIFY2(channelCount >= 1,
             qPrintable(QStringLiteral("channelCount=%1").arg(channelCount)));
    const QVariantList channels = readout.value(QStringLiteral("channels")).toList();
    QCOMPARE(channels.size(), channelCount);

    double channelSum = 0.0;
    for (const QVariant& entry : channels)
    {
        const QVariantMap channel = entry.toMap();
        QVERIFY(!channel.value(QStringLiteral("name")).toString().isEmpty());
        QVERIFY(!channel.value(QStringLiteral("displayName")).toString().isEmpty());
        const double coverage = channel.value(QStringLiteral("coverage")).toDouble();
        QVERIFY(coverage >= 0.0 && coverage <= 1.0);
        channelSum += coverage;
    }

    const double total = readout.value(QStringLiteral("totalInkCoverage")).toDouble();
    QVERIFY(qAbs(total - channelSum) < 1e-6);
    // The sampled point is inside the fixture's ink, so the readout is a measurement
    // rather than a plausible zero.
    QVERIFY(total > 0.0);
    QVERIFY(readout.value(QStringLiteral("reason")).toString().isEmpty());

    // Outside the page: a typed reason and available=false, not a zero readout.
    QVERIFY(host.requestInkCoverageAtPoint(-100.0, -100.0));
    QTRY_COMPARE_WITH_TIMEOUT(host.inkCoverageReadout().value(QStringLiteral("reason")).toString(),
                              QStringLiteral("ink-coverage/point-outside-page"), 30000);
    QVERIFY(!host.inkCoverageReadout().value(QStringLiteral("available")).toBool());
    QVERIFY(!host.inkCoverageReadout().contains(QStringLiteral("totalInkCoverage")));

    // No document: the request is refused outright.
    EditorHost empty;
    QVERIFY(!empty.requestInkCoverageAtPoint(10.0, 10.0));
}


void EditorHostTest::settingsRoundTripThroughTheOneStore()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath = directory.filePath(QStringLiteral("shell-settings.ini"));

    QSettings backing(settingsPath, QSettings::IniFormat);
    EditorSettings settings(&backing);
    EditorHost host(&settings);

    // A fresh store starts every option at its typed default.
    QVERIFY(settings.isOptionEnabled(QStringLiteral("antialiasing")));
    QVERIFY(!settings.isOptionEnabled(QStringLiteral("smoothPictures")));

    // Toggling through the command handler persists immediately.
    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionSmoothPictures")) != 0);
    QVERIFY(settings.isOptionEnabled(QStringLiteral("smoothPictures")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionAntialiasing")) != 0);
    QVERIFY(!settings.isOptionEnabled(QStringLiteral("antialiasing")));

    // The diagnostic-view commands toggle the same typed options.
    QVERIFY(host.invokeCommand(QStringLiteral("actionShow_Text_Blocks")) != 0);
    QVERIFY(settings.isOptionEnabled(QStringLiteral("showTextBlocks")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionDisplayRenderTimes")) != 0);
    QVERIFY(settings.isOptionEnabled(QStringLiteral("displayRenderTimes")));

    // The shared render presentation policy follows the store.
    const pdf::PDFRenderer::Features features = host.sessionForTest()->surfaces()->renderSettings().features;
    QVERIFY(features.testFlag(pdf::PDFRenderer::SmoothImages));
    QVERIFY(!features.testFlag(pdf::PDFRenderer::Antialiasing));
    QVERIFY(features.testFlag(pdf::PDFRenderer::DebugTextBlocks));
    QVERIFY(features.testFlag(pdf::PDFRenderer::DisplayTimes));

    // A second store over the same file reads the same typed state.
    QSettings reread(settingsPath, QSettings::IniFormat);
    QCOMPARE(reread.value(QStringLiteral("renderOptions/smoothPictures")).toString(), QStringLiteral("true"));
    QCOMPARE(reread.value(QStringLiteral("renderOptions/antialiasing")).toString(), QStringLiteral("false"));
    EditorSettings restarted(&reread);
    QVERIFY(restarted.isOptionEnabled(QStringLiteral("smoothPictures")));
    QVERIFY(!restarted.isOptionEnabled(QStringLiteral("antialiasing")));

    // The rows QML renders come from the same typed state.
    const QVariantList rows = settings.options();
    QCOMPARE(rows.size(), 8);
    bool sawSmoothPictures = false;
    for (const QVariant& row : rows)
    {
        const QVariantMap entry = row.toMap();
        QVERIFY(!entry.value(QStringLiteral("id")).toString().isEmpty());
        QVERIFY(!entry.value(QStringLiteral("label")).toString().isEmpty());
        if (entry.value(QStringLiteral("id")).toString() == QStringLiteral("smoothPictures"))
        {
            sawSmoothPictures = entry.value(QStringLiteral("enabled")).toBool();
        }
    }
    QVERIFY(sawSmoothPictures);
}

void EditorHostTest::settingsFactoryResetIsConfirmedAndComplete()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath = directory.filePath(QStringLiteral("shell-settings.ini"));

    QSettings backing(settingsPath, QSettings::IniFormat);
    EditorSettings settings(&backing);
    EditorHost host(&settings);

    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionSmoothPictures")) != 0);
    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionDisplayAnnotations")) != 0);
    QVERIFY(settings.isOptionEnabled(QStringLiteral("smoothPictures")));
    QVERIFY(!settings.isOptionEnabled(QStringLiteral("displayAnnotations")));

    // Nothing runs without the confirmation the command arms first.
    QVERIFY(!host.confirmSettingsReset());
    QVERIFY(host.invokeCommand(QStringLiteral("actionResetToFactorySettings")) != 0);
    QVERIFY(host.settingsResetPending());
    host.cancelSettingsReset();
    QVERIFY(!host.settingsResetPending());
    QVERIFY(settings.isOptionEnabled(QStringLiteral("smoothPictures")));

    QVERIFY(host.invokeCommand(QStringLiteral("actionResetToFactorySettings")) != 0);
    QVERIFY(host.settingsResetPending());
    QVERIFY(host.confirmSettingsReset());
    QVERIFY(!host.settingsResetPending());

    // Every option is back at its default and the owned keys are gone.
    QVERIFY(!settings.isOptionEnabled(QStringLiteral("smoothPictures")));
    QVERIFY(settings.isOptionEnabled(QStringLiteral("displayAnnotations")));
    QVERIFY(host.settingsNotice().contains(QStringLiteral("factory")));
    QSettings reread(settingsPath, QSettings::IniFormat);
    QVERIFY(!reread.contains(QStringLiteral("renderOptions/smoothPictures")));
    QVERIFY(!reread.contains(QStringLiteral("renderOptions/displayAnnotations")));

    const pdf::PDFRenderer::Features features = host.sessionForTest()->surfaces()->renderSettings().features;
    QVERIFY(features.testFlag(pdf::PDFRenderer::Antialiasing));
    QVERIFY(!features.testFlag(pdf::PDFRenderer::SmoothImages));
}

void EditorHostTest::invalidPersistedRenderOptionIsReportedAndReplaced()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath = directory.filePath(QStringLiteral("shell-settings.ini"));

    {
        QSettings seed(settingsPath, QSettings::IniFormat);
        seed.setValue(QStringLiteral("renderOptions/displayAnnotations"), QStringLiteral("sometimes"));
        seed.setValue(QStringLiteral("renderOptions/antialiasing"), QStringLiteral("false"));
        seed.sync();
        QCOMPARE(seed.status(), QSettings::NoError);
    }

    QSettings backing(settingsPath, QSettings::IniFormat);
    EditorSettings settings(&backing);
    EditorHost host(&settings);

    // The invalid value is replaced by its default, never coerced ...
    QVERIFY(settings.isOptionEnabled(QStringLiteral("displayAnnotations")));
    // ... while a valid persisted value is honoured.
    QVERIFY(!settings.isOptionEnabled(QStringLiteral("antialiasing")));

    // The notice names the setting and the rejected value, and the surface
    // that renders it reads the same notice through the host.
    QVERIFY(settings.notice().contains(QStringLiteral("displayAnnotations")));
    QVERIFY(settings.notice().contains(QStringLiteral("sometimes")));
    QVERIFY(host.settingsNotice().contains(QStringLiteral("displayAnnotations")));

    // The store is repaired on disk, not left invalid.
    QSettings reread(settingsPath, QSettings::IniFormat);
    QCOMPARE(reread.value(QStringLiteral("renderOptions/displayAnnotations")).toString(), QStringLiteral("true"));
}

void EditorHostTest::renderOptionsReachTheRendererAndTheFidelityIndicator()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("render-options.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    QSettings backing(directory.filePath(QStringLiteral("shell-settings.ini")), QSettings::IniFormat);
    EditorSettings settings(&backing);
    EditorHost host(&settings);

    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);
    QVERIFY(host.pageFidelityIsExact());

    // The override that deviates from the document's own view state is
    // reflected in the fidelity indicator and its reason ...
    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionIgnoreOptionalContentSettings")) != 0);
    QVERIFY(settings.isOptionEnabled(QStringLiteral("ignoreOptionalContentSettings")));
    QVERIFY(host.sessionForTest()->surfaces()->renderSettings().features.testFlag(pdf::PDFRenderer::IgnoreOptionalContent));
    QVERIFY(!host.pageFidelityIsExact());
    QVERIFY(host.pageFidelityReason().contains(QStringLiteral("optional content")));
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("approximate"), 30000);
    QVERIFY(host.previewFidelitySummary().contains(QStringLiteral("optional content")));

    // ... and turning it off restores the exact render.
    QVERIFY(host.invokeCommand(QStringLiteral("actionRenderOptionIgnoreOptionalContentSettings")) != 0);
    QVERIFY(!settings.isOptionEnabled(QStringLiteral("ignoreOptionalContentSettings")));
    QVERIFY(!host.sessionForTest()->surfaces()->renderSettings().features.testFlag(pdf::PDFRenderer::IgnoreOptionalContent));
    QVERIFY(host.pageFidelityIsExact());
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);

    // Presentation only: no option changed the document.
    QCOMPARE(host.documentShellStatus(), QStringLiteral("OPEN"));
    QVERIFY(!host.sessionForTest()->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty));
}

void EditorHostTest::collectDiagnosticsProducesTheAdrBundle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    EditorHost host;
    QCOMPARE(host.diagnosticsBundle().value(QStringLiteral("status")).toString(), QStringLiteral("idle"));

    // A non-local parent is refused with a typed failure; nothing is written.
    QVERIFY(!host.collectDiagnosticsFileUrl(QUrl(QStringLiteral("https://example.test/bundles"))));
    QCOMPARE(host.diagnosticsBundle().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    QVERIFY(!host.diagnosticsBundle().value(QStringLiteral("error")).toString().isEmpty());

    QVERIFY(host.collectDiagnosticsFileUrl(QUrl::fromLocalFile(directory.path())));
    const QVariantMap bundle = host.diagnosticsBundle();
    QCOMPARE(bundle.value(QStringLiteral("status")).toString(), QStringLiteral("ready"));
    QVERIFY(bundle.value(QStringLiteral("fileCount")).toInt() > 0);

    const QString bundleDirectory = bundle.value(QStringLiteral("directory")).toString();
    QVERIFY(QDir(bundleDirectory).exists());
    QVERIFY(QDir(bundleDirectory).dirName().startsWith(QStringLiteral("loop-diagnostics-editor-")));

    QFile manifestFile(QDir(bundleDirectory).filePath(QStringLiteral("manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    QCOMPARE(manifest.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(manifest.value(QStringLiteral("application")).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("editor"));
    QVERIFY(!manifest.value(QStringLiteral("files")).toArray().isEmpty());
    QVERIFY(QFile::exists(QDir(bundleDirectory).filePath(QStringLiteral("README.txt"))));

    // The bundle never carries a PDF, however the collection was started.
    QDirIterator pdfs(bundleDirectory, QStringList{ QStringLiteral("*.pdf") }, QDir::Files, QDirIterator::Subdirectories);
    QVERIFY(!pdfs.hasNext());
}

/// Issue #232. Extraction must refuse to overwrite an existing file up front and
/// leave that file byte-identical; a silent clobber would destroy the operator's
/// own file.
void EditorHostTest::imageExtractionRefusesToOverwriteAnExistingFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // A one-page document carrying one 2x2 RGB image XObject and no text.
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));

    QByteArray imageData;
    for (int pixel = 0; pixel < 4; ++pixel)
    {
        imageData.append('\xff');
        imageData.append('\0');
        imageData.append('\0');
    }

    pdf::PDFDictionary imageDictionary;
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Image"));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Width"), pdf::PDFObject::createInteger(2));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Height"), pdf::PDFObject::createInteger(2));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"), pdf::PDFObject::createName("DeviceRGB"));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("BitsPerComponent"), pdf::PDFObject::createInteger(8));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH), pdf::PDFObject::createInteger(imageData.size()));
    pdf::PDFStream imageStream(std::move(imageDictionary), std::move(imageData));
    const pdf::PDFObjectReference imageReference =
        builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(imageStream)));

    const QByteArray content = "q 40 0 0 40 80 80 cm /Im0 Do Q";
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(content.size()));
    pdf::PDFStream contentStream(std::move(contentDictionary), QByteArray(content));
    const pdf::PDFObjectReference contentReference =
        builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(contentStream)));

    pdf::PDFDictionary xObjects;
    xObjects.addEntry(pdf::PDFInplaceOrMemoryString("Im0"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObjects))));

    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));

    const QString path = directory.filePath(QStringLiteral("extract-source.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    const QString existing = directory.filePath(QStringLiteral("page-1-image-1.png"));
    QFile planted(existing);
    QVERIFY(planted.open(QIODevice::WriteOnly));
    planted.write("keep-me");
    planted.close();

    QVERIFY(!host.extractCurrentPageImagesFileUrl(QUrl::fromLocalFile(directory.path())));

    QFile untouched(existing);
    QVERIFY(untouched.open(QIODevice::ReadOnly));
    QCOMPARE(untouched.readAll(), QByteArray("keep-me"));
}

void EditorHostTest::bookmarkCommandsWriteTheOutlineThroughTheCatalog()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = writePagesFixture(directory.filePath(QStringLiteral("bookmarks.pdf")), 3);
    QVERIFY(!path.isEmpty());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    QSignalSpy finished(&host.sessionForTest()->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);
    auto* model = qobject_cast<QuickDocumentModel*>(host.documentModel());
    QVERIFY(model);
    QVERIFY(!model->hasOutline());
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionGotoNextBookmark")));

    // The viewport has not placed a page yet, so there is no page the operator
    // is on. The command refuses with its own reason instead of blaming the
    // document's page range, which is not what is wrong.
    const quint64 unplaced = host.invokeCommand(QStringLiteral("actionBookmarkPage"),
                                                { { QStringLiteral("title"), QStringLiteral("Nowhere") } });
    QVERIFY(unplaced != 0);
    const std::optional<pdfinteraction::CommandResult> unplacedResult = terminalResultFor(finished, unplaced);
    QVERIFY(unplacedResult.has_value());
    QCOMPARE(unplacedResult->state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(unplacedResult->typedError, QStringLiteral("bookmark/no-current-page"));

    // Once the canvas reports its geometry, the operator is on the first page.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    host.goToPage(0);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), 0, 15000);
    waitForSurfacesToSettle(host);

    // Bookmark the page the operator is on; the outline becomes the bookmark,
    // through the catalog and the facade gateway into Core.
    const quint64 bookmark = host.invokeCommand(QStringLiteral("actionBookmarkPage"),
                                                { { QStringLiteral("title"), QStringLiteral("Start") } });
    QVERIFY(bookmark != 0);
    const std::optional<pdfinteraction::CommandResult> bookmarkResult = terminalResultFor(finished, bookmark);
    QVERIFY(bookmarkResult.has_value());
    QVERIFY2(bookmarkResult->state == pdfinteraction::CommandTerminalState::Completed,
             qPrintable(bookmarkResult->typedError));
    QVERIFY(bookmarkResult->typedError.isEmpty());

    const pdf::PDFDocumentPointer document = host.sessionForTest()->context().getDocumentPointer();
    QString typedError;
    std::optional<QList<pdf::PDFBookmarkRecord>> records =
        pdf::PDFAuthoringEditWriter::readOutline(document.data(), typedError);
    QVERIFY2(records.has_value(), qPrintable(typedError));
    QCOMPARE(records->size(), 1);
    QCOMPARE(records->at(0).title, QStringLiteral("Start"));
    QCOMPARE(records->at(0).pageIndex, 0);
    QVERIFY(model->hasOutline());
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionGotoNextBookmark")));

    // The same command on the same page removes it again, so the shell never
    // silently accumulates duplicates.
    const quint64 toggledOff = host.invokeCommand(QStringLiteral("actionBookmarkPage"));
    QVERIFY(toggledOff != 0);
    const std::optional<pdfinteraction::CommandResult> toggleResult = terminalResultFor(finished, toggledOff);
    QVERIFY(toggleResult.has_value());
    QCOMPARE(toggleResult->state, pdfinteraction::CommandTerminalState::Completed);
    records = pdf::PDFAuthoringEditWriter::readOutline(host.sessionForTest()->context().getDocumentPointer().data(),
                                                       typedError);
    QVERIFY(records.has_value());
    QVERIFY(records->isEmpty());

    // Auto-generate fills in every page that has none.
    const quint64 generated = host.invokeCommand(QStringLiteral("actionBookmarkAutoGenerate"));
    QVERIFY(generated != 0);
    const std::optional<pdfinteraction::CommandResult> generatedResult = terminalResultFor(finished, generated);
    QVERIFY(generatedResult.has_value());
    QCOMPARE(generatedResult->state, pdfinteraction::CommandTerminalState::Completed);
    records = pdf::PDFAuthoringEditWriter::readOutline(host.sessionForTest()->context().getDocumentPointer().data(),
                                                       typedError);
    QVERIFY(records.has_value());
    QCOMPARE(records->size(), 3);
    QCOMPARE(records->at(0).title, QStringLiteral("Page 1"));
    QCOMPARE(records->at(2).title, QStringLiteral("Page 3"));

    // A second run has nothing to generate: it refuses with a visible reason and
    // writes no revision.
    const pdf::PDFRevisionIdentity revisionBefore = host.sessionForTest()->facade().currentRevision();
    const quint64 again = host.invokeCommand(QStringLiteral("actionBookmarkAutoGenerate"));
    QVERIFY(again != 0);
    const std::optional<pdfinteraction::CommandResult> againResult = terminalResultFor(finished, again);
    QVERIFY(againResult.has_value());
    QCOMPARE(againResult->state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(againResult->typedError, QStringLiteral("bookmark/nothing-to-generate"));
    QCOMPARE(host.sessionForTest()->facade().currentRevision(), revisionBefore);

    // Bookmark navigation moves the viewport and edits nothing.
    host.goToPage(0);
    const quint64 next = host.invokeCommand(QStringLiteral("actionGotoNextBookmark"));
    QVERIFY(next != 0);
    const std::optional<pdfinteraction::CommandResult> nextResult = terminalResultFor(finished, next);
    QVERIFY(nextResult.has_value());
    QCOMPARE(nextResult->state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(host.currentPage(), 1);
    QCOMPARE(host.sessionForTest()->facade().currentRevision(), revisionBefore);

    const quint64 previous = host.invokeCommand(QStringLiteral("actionGotoPreviousBookmark"));
    QVERIFY(previous != 0);
    const std::optional<pdfinteraction::CommandResult> previousResult = terminalResultFor(finished, previous);
    QVERIFY(previousResult.has_value());
    QCOMPARE(previousResult->state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(host.currentPage(), 0);
    QCOMPARE(host.sessionForTest()->facade().currentRevision(), revisionBefore);
}

void EditorHostTest::bookmarkImportExportRoundTripsAndRefusesAStaleRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = writePagesFixture(directory.filePath(QStringLiteral("source.pdf")), 3);
    const QString targetPath = writePagesFixture(directory.filePath(QStringLiteral("target.pdf")), 3);
    QVERIFY(!sourcePath.isEmpty());
    QVERIFY(!targetPath.isEmpty());

    EditorHost source(nullptr, nullptr, m_fixtureRecipes->path());
    source.openFileUrl(QUrl::fromLocalFile(sourcePath));
    QTRY_VERIFY_WITH_TIMEOUT(source.hasDocument(), 15000);
    // The operator is on the first page when the bookmark commands run.
    source.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    source.goToPage(0);
    QTRY_COMPARE_WITH_TIMEOUT(source.currentPage(), 0, 15000);
    waitForSurfacesToSettle(source);

    QSignalSpy finished(&source.sessionForTest()->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);
    QVERIFY(source.invokeCommand(QStringLiteral("actionBookmarkPage"),
                                 { { QStringLiteral("title"), QStringLiteral("Chapter one") } }) != 0);
    QVERIFY(source.invokeCommand(QStringLiteral("actionBookmarkAutoGenerate")) != 0);

    const pdf::PDFDocumentPointer sourceDocument = source.sessionForTest()->context().getDocumentPointer();
    QString typedError;
    const std::optional<QList<pdf::PDFBookmarkRecord>> sourceRecords =
        pdf::PDFAuthoringEditWriter::readOutline(sourceDocument.data(), typedError);
    QVERIFY2(sourceRecords.has_value(), qPrintable(typedError));
    QCOMPARE(sourceRecords->size(), 3);

    // Export through the catalog writes the interchange file.
    const QString bookmarkPath = directory.filePath(QStringLiteral("bookmarks.json"));
    const quint64 exported = source.invokeCommand(QStringLiteral("actionBookmarkExport"),
                                                  { { QStringLiteral("path"), bookmarkPath } });
    QVERIFY(exported != 0);
    const std::optional<pdfinteraction::CommandResult> exportedResult = terminalResultFor(finished, exported);
    QVERIFY(exportedResult.has_value());
    QCOMPARE(exportedResult->state, pdfinteraction::CommandTerminalState::Completed);

    QFile exportedFile(bookmarkPath);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    const QByteArray exportedBytes = exportedFile.readAll();
    exportedFile.close();
    const std::optional<pdf::PDFBookmarkFile> parsedFile = pdf::PDFBookmarkFile::fromJson(exportedBytes, typedError);
    QVERIFY2(parsedFile.has_value(), qPrintable(typedError));
    QCOMPARE(parsedFile->bookmarks, *sourceRecords);

    // Import into a different document: the outline becomes the file, and the
    // records survive without loss.
    EditorHost target(nullptr, nullptr, m_fixtureRecipes->path());
    target.openFileUrl(QUrl::fromLocalFile(targetPath));
    QTRY_VERIFY_WITH_TIMEOUT(target.hasDocument(), 15000);
    target.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    target.goToPage(0);
    QTRY_COMPARE_WITH_TIMEOUT(target.currentPage(), 0, 15000);
    waitForSurfacesToSettle(target);

    QSignalSpy targetFinished(&target.sessionForTest()->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);
    const quint64 imported = target.invokeCommand(QStringLiteral("actionBookmarkImport"),
                                                  { { QStringLiteral("path"), bookmarkPath } });
    QVERIFY(imported != 0);
    const std::optional<pdfinteraction::CommandResult> importedResult = terminalResultFor(targetFinished, imported);
    QVERIFY(importedResult.has_value());
    QCOMPARE(importedResult->state, pdfinteraction::CommandTerminalState::Completed);

    const std::optional<QList<pdf::PDFBookmarkRecord>> targetRecords =
        pdf::PDFAuthoringEditWriter::readOutline(target.sessionForTest()->context().getDocumentPointer().data(),
                                                 typedError);
    QVERIFY(targetRecords.has_value());
    QCOMPARE(*targetRecords, *sourceRecords);

    // A malformed file is refused with a typed reason and changes nothing.
    const QString malformedPath = directory.filePath(QStringLiteral("malformed.json"));
    QFile malformed(malformedPath);
    QVERIFY(malformed.open(QIODevice::WriteOnly));
    malformed.write("{\"schema_version\":1,\"bookmarks\":[{\"page\":0}]}");
    malformed.close();
    const pdf::PDFRevisionIdentity revisionBefore = target.sessionForTest()->facade().currentRevision();
    const quint64 refused = target.invokeCommand(QStringLiteral("actionBookmarkImport"),
                                                 { { QStringLiteral("path"), malformedPath } });
    QVERIFY(refused != 0);
    const std::optional<pdfinteraction::CommandResult> refusedResult = terminalResultFor(targetFinished, refused);
    QVERIFY(refusedResult.has_value());
    QCOMPARE(refusedResult->state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(refusedResult->typedError, QStringLiteral("bookmark/invalid-file"));
    QCOMPARE(target.sessionForTest()->facade().currentRevision(), revisionBefore);

    // The failure case of #233: an edit carrying a revision the document has
    // left is refused with a visible reason and writes nothing.
    const QString displayed = target.sessionForTest()->facade().currentRevision().toString();
    QVERIFY(target.invokeCommand(QStringLiteral("actionBookmarkPage")) != 0);
    QVERIFY(target.sessionForTest()->facade().currentRevision().toString() != displayed);

    const quint64 stale = target.invokeCommand(QStringLiteral("actionBookmarkPage"),
                                               { { QStringLiteral("revision"), displayed } });
    QVERIFY(stale != 0);
    const std::optional<pdfinteraction::CommandResult> staleResult = terminalResultFor(targetFinished, stale);
    QVERIFY(staleResult.has_value());
    QCOMPARE(staleResult->state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(staleResult->typedError, QStringLiteral("edit/stale-revision"));
}

void EditorHostTest::hyperlinkCommandsWriteEveryDestinationVariant_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("destinationType");
    QTest::addColumn<bool>("inheritZoom");

    QTest::newRow("Fit") << QStringLiteral("actionCreateHyperlinkToThisPDFFit") << int(pdf::DestinationType::Fit) << false;
    QTest::newRow("FitB") << QStringLiteral("actionCreateHyperlinkToThisPDFFitB") << int(pdf::DestinationType::FitB) << false;
    QTest::newRow("FitBH") << QStringLiteral("actionCreateHyperlinkToThisPDFFitBH") << int(pdf::DestinationType::FitBH) << false;
    QTest::newRow("FitBV") << QStringLiteral("actionCreateHyperlinkToThisPDFFitBV") << int(pdf::DestinationType::FitBV) << false;
    QTest::newRow("FitH") << QStringLiteral("actionCreateHyperlinkToThisPDFFitH") << int(pdf::DestinationType::FitH) << false;
    QTest::newRow("FitR") << QStringLiteral("actionCreateHyperlinkToThisPDFFitR") << int(pdf::DestinationType::FitR) << false;
    QTest::newRow("FitV") << QStringLiteral("actionCreateHyperlinkToThisPDFFitV") << int(pdf::DestinationType::FitV) << false;
    QTest::newRow("XYZ") << QStringLiteral("actionCreateHyperlinkToThisPDFXYZ") << int(pdf::DestinationType::XYZ) << false;
    QTest::newRow("XYZInheritZoom") << QStringLiteral("actionCreateHyperlinkToThisPDFXYZInheritZoom")
                                    << int(pdf::DestinationType::XYZ) << true;
}

void EditorHostTest::hyperlinkCommandsWriteEveryDestinationVariant()
{
    QFETCH(QString, command);
    QFETCH(int, destinationType);
    QFETCH(bool, inheritZoom);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = writePagesFixture(directory.filePath(QStringLiteral("hyperlinks.pdf")), 2);
    QVERIFY(!path.isEmpty());

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.isCommandEnabled(command));

    QSignalSpy finished(&host.sessionForTest()->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);
    const quint64 invocation = host.invokeCommand(command,
                                                  { { QStringLiteral("page"), 0 },
                                                    { QStringLiteral("left"), 10.0 },
                                                    { QStringLiteral("bottom"), 20.0 },
                                                    { QStringLiteral("right"), 60.0 },
                                                    { QStringLiteral("top"), 40.0 },
                                                    { QStringLiteral("targetPage"), 1 },
                                                    { QStringLiteral("targetLeft"), 5.0 },
                                                    { QStringLiteral("targetBottom"), 6.0 },
                                                    { QStringLiteral("targetRight"), 70.0 },
                                                    { QStringLiteral("targetTop"), 90.0 },
                                                    { QStringLiteral("zoom"), 1.5 } });
    QVERIFY(invocation != 0);
    const std::optional<pdfinteraction::CommandResult> result = terminalResultFor(finished, invocation);
    QVERIFY(result.has_value());
    QCOMPARE(result->state, pdfinteraction::CommandTerminalState::Completed);

    // Read back through Core: the link annotation carries the destination the
    // command named, and the destination carries its own payload.
    const pdf::PDFDocumentPointer document = host.sessionForTest()->context().getDocumentPointer();
    const pdf::PDFLinkAnnotation* link = linkOnPage(document.data(), 0);
    QVERIFY(link);
    QCOMPARE(link->getRectangle(), QRectF(QPointF(10, 20), QPointF(60, 40)));

    const pdf::PDFActionGoTo* goTo = dynamic_cast<const pdf::PDFActionGoTo*>(link->getAction());
    QVERIFY(goTo);
    const pdf::PDFDestination& destination = goTo->getDestination();
    QCOMPARE(int(destination.getDestinationType()), destinationType);
    QCOMPARE(destinationPageIndex(document.data(), destination), 1);

    switch (destination.getDestinationType())
    {
        case pdf::DestinationType::Fit:
        case pdf::DestinationType::FitB:
            QVERIFY(!destination.hasLeft());
            QVERIFY(!destination.hasTop());
            break;

        case pdf::DestinationType::FitH:
        case pdf::DestinationType::FitBH:
            QCOMPARE(destination.getTop(), 90.0);
            break;

        case pdf::DestinationType::FitV:
        case pdf::DestinationType::FitBV:
            QCOMPARE(destination.getLeft(), 5.0);
            break;

        case pdf::DestinationType::FitR:
            QCOMPARE(destination.getLeft(), 5.0);
            QCOMPARE(destination.getBottom(), 6.0);
            QCOMPARE(destination.getRight(), 70.0);
            QCOMPARE(destination.getTop(), 90.0);
            break;

        case pdf::DestinationType::XYZ:
            QCOMPARE(destination.getLeft(), 5.0);
            QCOMPARE(destination.getTop(), 90.0);
            // The inherit-zoom variant writes the PDF null zoom, which reads
            // back as "no zoom"; the plain XYZ keeps the requested zoom.
            QCOMPARE(destination.hasZoom(), !inheritZoom);
            if (!inheritZoom)
            {
                QCOMPARE(destination.getZoom(), 1.5);
            }
            break;

        case pdf::DestinationType::Invalid:
        case pdf::DestinationType::Named:
            QFAIL("the command must never write an invalid destination");
    }
}

void EditorHostTest::restrictedDocumentCannotEditBookmarksOrLinks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("restricted-bookmarks.pdf"));
    // AES-256 with no user permissions: the user may read but not modify.
    QVERIFY(writeEncryptedFixture(path, pdf::PDFSecurityHandlerFactory::AES_256, QStringLiteral("fixture-user"), 0));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.passwordRequestId() != 0, 10000);
    QVERIFY(host.submitPassword(host.passwordRequestId(), QStringLiteral("fixture-user")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    QVERIFY(host.sessionForTest()->facade().permissions().encrypted);
    QVERIFY(!host.sessionForTest()->facade().permissions().modify);
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionBookmarkPage")));
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionBookmarkAutoGenerate")));
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionBookmarkImport")));
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionCreateHyperlink")));
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionCreateHyperlinkToThisPDFFit")));

    // A disabled command starts nothing at all: there is no path from the menu
    // to a half-written outline.
    QCOMPARE(host.invokeCommand(QStringLiteral("actionBookmarkPage")), quint64(0));
    QCOMPARE(host.invokeCommand(QStringLiteral("actionCreateHyperlink"),
                                { { QStringLiteral("uri"), QStringLiteral("https://example.org") } }),
             quint64(0));
}

void EditorHostTest::formValueEditRoutesThroughTheCatalogIntoCoreAndProducesANewRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("form.pdf"));
    QVERIFY(writeFormFixtureDocument(path));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    const pdf::PDFRevisionIdentity before = session->facade().currentRevision();
    QVERIFY(before.isValid());
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionSetFormFieldValue")));
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionEditDocumentMetadata")));
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionAddAttachment")));

    auto* model = qobject_cast<QuickDocumentModel*>(host.documentModel());
    QVERIFY(model);
    QVERIFY(model->hasForm());
    QCOMPARE(readFormFieldValue(session->context().getDocument(), QStringLiteral("LoopText")), QStringLiteral("original"));

    QSignalSpy finished(&session->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);

    // Text value: the catalog route into Core, one new revision, one dirty document.
    QVariantMap parameters;
    parameters.insert(QStringLiteral("field"), QStringLiteral("LoopText"));
    parameters.insert(QStringLiteral("value"), QStringLiteral("typed in the shell"));
    parameters.insert(QStringLiteral("revision"), before.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters) != 0);
    QCOMPARE(finished.count(), 1);
    const pdfinteraction::CommandResult textResult = finished.last().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(textResult.state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(textResult.typedError, QString());

    const pdf::PDFRevisionIdentity afterText = session->facade().currentRevision();
    QVERIFY(afterText != before);
    QCOMPARE(session->facade().state(), pdfinteraction::DocumentState::Ready);
    QVERIFY(session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty));
    QCOMPARE(host.documentShellStatus(), QStringLiteral("MODIFIED"));
    QCOMPARE(model->revision(), afterText.toString());
    QVERIFY(model->modified());
    QCOMPARE(readFormFieldValue(session->context().getDocument(), QStringLiteral("LoopText")), QStringLiteral("typed in the shell"));

    // Checkbox: the same route, and the value lands as the widget's on-state name.
    parameters.clear();
    parameters.insert(QStringLiteral("field"), QStringLiteral("LoopCheck"));
    parameters.insert(QStringLiteral("checked"), true);
    parameters.insert(QStringLiteral("revision"), afterText.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters) != 0);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);
    bool isName = false;
    QCOMPARE(readFormFieldValue(session->context().getDocument(), QStringLiteral("LoopCheck"), &isName), QStringLiteral("Yes"));
    QVERIFY(isName);

    // Metadata: the same gateway, and the properties the model presents follow.
    parameters.clear();
    parameters.insert(QStringLiteral("title"), QStringLiteral("Shell title"));
    parameters.insert(QStringLiteral("revision"), session->facade().currentRevision().toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionEditDocumentMetadata"), parameters) != 0);
    QCOMPARE(finished.count(), 3);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(model->title(), QStringLiteral("Shell title"));
    QCOMPARE(session->context().getDocument()->getInfo()->title, QStringLiteral("Shell title"));

    // A signature field is refused, and a refusal changes nothing at all.
    const pdf::PDFRevisionIdentity beforeRefusal = session->facade().currentRevision();
    parameters.clear();
    parameters.insert(QStringLiteral("field"), QStringLiteral("LoopSignature"));
    parameters.insert(QStringLiteral("value"), QStringLiteral("not a signature"));
    parameters.insert(QStringLiteral("revision"), beforeRefusal.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters) != 0);
    QCOMPARE(finished.count(), 4);
    const pdfinteraction::CommandResult refused = finished.last().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(refused.state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(refused.typedError, QStringLiteral("form/unsupported-field-type"));
    QCOMPARE(session->facade().currentRevision(), beforeRefusal);
}

void EditorHostTest::authoringEditRefusesAStaleRevisionAndChangesNothing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("stale-form.pdf"));
    QVERIFY(writeFormFixtureDocument(path));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    const pdf::PDFRevisionIdentity first = session->facade().currentRevision();
    QVERIFY(first.isValid());
    QSignalSpy finished(&session->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);

    QVariantMap parameters;
    parameters.insert(QStringLiteral("field"), QStringLiteral("LoopText"));
    parameters.insert(QStringLiteral("value"), QStringLiteral("the first edit"));
    parameters.insert(QStringLiteral("revision"), first.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters) != 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);

    const pdf::PDFRevisionIdentity current = session->facade().currentRevision();
    QVERIFY(current != first);

    // The edit begun against the superseded revision is refused by the fence.
    parameters.clear();
    parameters.insert(QStringLiteral("field"), QStringLiteral("LoopText"));
    parameters.insert(QStringLiteral("value"), QStringLiteral("stale edit"));
    parameters.insert(QStringLiteral("revision"), first.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters) != 0);
    QCOMPARE(finished.count(), 2);
    const pdfinteraction::CommandResult stale = finished.last().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(stale.state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(stale.typedError, QStringLiteral("document/stale-revision"));
    QCOMPARE(session->facade().currentRevision(), current);
    QCOMPARE(readFormFieldValue(session->context().getDocument(), QStringLiteral("LoopText")), QStringLiteral("the first edit"));

    // An edit naming no revision at all never reaches the fence: the catalog
    // refuses the missing declared parameter, and the refusal is reported.
    parameters.clear();
    parameters.insert(QStringLiteral("field"), QStringLiteral("LoopText"));
    parameters.insert(QStringLiteral("value"), QStringLiteral("unfenced edit"));
    QCOMPARE(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters), quint64(0));
    QCOMPARE(finished.count(), 3);
    const pdfinteraction::CommandResult unfenced = finished.last().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(unfenced.state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(unfenced.typedError, QStringLiteral("command/missing-parameter"));
    QCOMPARE(readFormFieldValue(session->context().getDocument(), QStringLiteral("LoopText")), QStringLiteral("the first edit"));
}

void EditorHostTest::xfaOnlyFormIsReportedAsUnsupportedThroughTheShell()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("xfa-form.pdf"));
    QVERIFY(writeXfaOnlyFixtureDocument(path));

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    const pdf::PDFRevisionIdentity before = session->facade().currentRevision();
    QSignalSpy finished(&session->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);

    QVariantMap parameters;
    parameters.insert(QStringLiteral("field"), QStringLiteral("AnyField"));
    parameters.insert(QStringLiteral("value"), QStringLiteral("value"));
    parameters.insert(QStringLiteral("revision"), before.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionSetFormFieldValue"), parameters) != 0);
    QCOMPARE(finished.count(), 1);
    const pdfinteraction::CommandResult refused = finished.last().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(refused.state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(refused.typedError, QStringLiteral("form/xfa-unsupported"));
    QCOMPARE(session->facade().currentRevision(), before);
    QVERIFY(!session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty));
    QCOMPARE(host.documentShellStatus(), QStringLiteral("OPEN"));
}

void EditorHostTest::attachmentEditsAndExtractionRouteThroughTheShell()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTemporaryDir extractionDirectory;
    QVERIFY(extractionDirectory.isValid());

    const QString path = directory.filePath(QStringLiteral("plain.pdf"));
    {
        pdf::PDFDocumentBuilder builder;
        builder.appendPage(QRectF(0, 0, 200, 200));
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    const QString textSource = directory.filePath(QStringLiteral("notes.txt"));
    {
        QFile source(textSource);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(QByteArrayLiteral("attachment payload")), qint64(18));
    }
    const QString executableSource = directory.filePath(QStringLiteral("setup.exe"));
    {
        QFile source(executableSource);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArrayLiteral("MZ not really an executable"));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    auto* model = qobject_cast<QuickDocumentModel*>(host.documentModel());
    QVERIFY(model);
    QVERIFY(!model->hasAttachments());

    QSignalSpy finished(&session->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);
    QVariantMap parameters;
    parameters.insert(QStringLiteral("name"), QStringLiteral("notes.txt"));
    parameters.insert(QStringLiteral("path"), textSource);
    parameters.insert(QStringLiteral("description"), QStringLiteral("Reviewer notes"));
    parameters.insert(QStringLiteral("revision"), session->facade().currentRevision().toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionAddAttachment"), parameters) != 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);
    QVERIFY(model->hasAttachments());
    QVERIFY(session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty));

    // Extraction writes through the atomic writer; a text file is not refused.
    QVERIFY(host.extractAttachmentFileUrl(QUrl::fromLocalFile(extractionDirectory.path()), QStringLiteral("notes.txt")));
    const QVariantMap extraction = host.attachmentExtraction();
    QCOMPARE(extraction.value(QStringLiteral("status")).toString(), QStringLiteral("written"));
    QCOMPARE(extraction.value(QStringLiteral("fileName")).toString(), QStringLiteral("notes.txt"));
    QCOMPARE(extraction.value(QStringLiteral("autoOpenRefused")).toBool(), false);
    QCOMPARE(extraction.value(QStringLiteral("typedError")).toString(), QString());
    {
        QFile extracted(extractionDirectory.filePath(QStringLiteral("notes.txt")));
        QVERIFY(extracted.open(QIODevice::ReadOnly));
        QCOMPARE(extracted.readAll(), QByteArrayLiteral("attachment payload"));
    }

    // An executable-type attachment is extracted, and must not be auto-opened.
    parameters.clear();
    parameters.insert(QStringLiteral("name"), QStringLiteral("setup.exe"));
    parameters.insert(QStringLiteral("path"), executableSource);
    parameters.insert(QStringLiteral("revision"), session->facade().currentRevision().toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionAddAttachment"), parameters) != 0);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);
    QVERIFY(host.extractAttachmentFileUrl(QUrl::fromLocalFile(extractionDirectory.path()), QStringLiteral("setup.exe")));
    QCOMPARE(host.attachmentExtraction().value(QStringLiteral("status")).toString(), QStringLiteral("written"));
    QCOMPARE(host.attachmentExtraction().value(QStringLiteral("autoOpenRefused")).toBool(), true);

    // Removal unlists the attachment, and the model follows both ways.
    parameters.clear();
    parameters.insert(QStringLiteral("name"), QStringLiteral("notes.txt"));
    parameters.insert(QStringLiteral("revision"), session->facade().currentRevision().toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionRemoveAttachment"), parameters) != 0);
    QCOMPARE(finished.count(), 3);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);
    QVERIFY(model->hasAttachments());

    parameters.clear();
    parameters.insert(QStringLiteral("name"), QStringLiteral("setup.exe"));
    parameters.insert(QStringLiteral("revision"), session->facade().currentRevision().toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionRemoveAttachment"), parameters) != 0);
    QCOMPARE(finished.count(), 4);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().state, pdfinteraction::CommandTerminalState::Completed);
    QVERIFY(!model->hasAttachments());

    // Removing what is no longer there is a typed refusal, not a success.
    parameters.clear();
    parameters.insert(QStringLiteral("name"), QStringLiteral("setup.exe"));
    parameters.insert(QStringLiteral("revision"), session->facade().currentRevision().toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionRemoveAttachment"), parameters) != 0);
    QCOMPARE(finished.count(), 5);
    QCOMPARE(finished.last().at(0).value<pdfinteraction::CommandResult>().typedError, QStringLiteral("attachment/not-found"));

    // Extraction of an unknown name is refused with the Core writer's code.
    QVERIFY(!host.extractAttachmentFileUrl(QUrl::fromLocalFile(extractionDirectory.path()), QStringLiteral("absent.txt")));
    QCOMPARE(host.attachmentExtraction().value(QStringLiteral("status")).toString(), QStringLiteral("refused"));
    QCOMPARE(host.attachmentExtraction().value(QStringLiteral("typedError")).toString(), QStringLiteral("attachment/not-found"));
}

void EditorHostTest::sendByEMailHandsOverOnlyThePublishedRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("mail.pdf"));
    {
        pdf::PDFDocumentBuilder builder;
        builder.appendPage(QRectF(0, 0, 200, 200));
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host(nullptr, nullptr, m_fixtureRecipes->path());
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionSend_by_E_Mail")));

    QSignalSpy mailSpy(&host, &EditorHost::documentMailRequested);
    QSignalSpy finished(&session->catalog(), &pdfinteraction::CommandCatalog::invocationFinished);
    const pdf::PDFRevisionIdentity published = session->facade().currentRevision();

    // The save command finishes its own invocation too, so results are read by
    // command id rather than by position.
    const auto lastResultFor = [&finished](const QString& commandId)
    {
        for (int index = finished.count() - 1; index >= 0; --index)
        {
            const pdfinteraction::CommandResult result = finished.at(index).at(0).value<pdfinteraction::CommandResult>();
            if (result.command == commandId)
            {
                return result;
            }
        }
        return pdfinteraction::CommandResult();
    };

    QVERIFY(host.invokeCommand(QStringLiteral("actionSend_by_E_Mail")) != 0);
    QCOMPARE(lastResultFor(QStringLiteral("actionSend_by_E_Mail")).state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(mailSpy.count(), 1);
    QCOMPARE(mailSpy.last().at(0).toString(), path);
    QCOMPARE(mailSpy.last().at(1).toString(), published.toString());

    // An unsaved edit makes the file on disk a different revision than the one
    // on screen, so the hand-off is refused rather than mailed as current.
    QVariantMap parameters;
    parameters.insert(QStringLiteral("title"), QStringLiteral("Unsaved title"));
    parameters.insert(QStringLiteral("revision"), published.toString());
    QVERIFY(host.invokeCommand(QStringLiteral("actionEditDocumentMetadata"), parameters) != 0);
    QCOMPARE(lastResultFor(QStringLiteral("actionEditDocumentMetadata")).state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(host.documentShellStatus(), QStringLiteral("MODIFIED"));

    QVERIFY(host.invokeCommand(QStringLiteral("actionSend_by_E_Mail")) != 0);
    const pdfinteraction::CommandResult refused = lastResultFor(QStringLiteral("actionSend_by_E_Mail"));
    QCOMPARE(refused.state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(refused.typedError, QStringLiteral("mail/unsaved-changes"));
    QCOMPARE(mailSpy.count(), 1);

    // Saving publishes the revision the shell was showing, and the hand-off
    // works again with the saved file and that same revision.
    const QString savedPath = directory.filePath(QStringLiteral("mail-saved.pdf"));
    const pdf::PDFRevisionIdentity edited = session->facade().currentRevision();
    host.saveAsFileUrl(QUrl::fromLocalFile(savedPath));
    QTRY_COMPARE_WITH_TIMEOUT(host.documentShellStatus(), QStringLiteral("OUTPUT_SAVED"), 15000);

    QVERIFY(host.invokeCommand(QStringLiteral("actionSend_by_E_Mail")) != 0);
    QCOMPARE(lastResultFor(QStringLiteral("actionSend_by_E_Mail")).state, pdfinteraction::CommandTerminalState::Completed);
    QCOMPARE(mailSpy.count(), 2);
    QCOMPARE(mailSpy.last().at(0).toString(), savedPath);
    QCOMPARE(mailSpy.last().at(1).toString(), edited.toString());
    QCOMPARE(session->facade().currentRevision(), edited);
}

QTEST_GUILESS_MAIN(EditorHostTest)
#include "tst_editorhosttest.moc"
