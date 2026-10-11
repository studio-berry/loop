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

// Core-level evidence for ADR-014's direct authoring edits (epic #231, issue
// #242): the form value, attachment and metadata writers, and the attachment
// extraction path through the atomic writer. The interaction layer's fence and
// the catalog route are pinned by UnitTestsEditorHost; this file pins what the
// writers themselves do to the document, and what they refuse to do.

#include <QtTest>

#include <QBuffer>
#include <QDir>
#include <QTemporaryDir>

#include "pdfcatalog.h"
#include "pdfdirectedit.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdfencoding.h"
#include "pdffile.h"
#include "pdfform.h"

namespace
{

pdf::PDFObject arrayOf(std::initializer_list<double> values)
{
    std::vector<pdf::PDFObject> objects;
    objects.reserve(values.size());
    for (const double value : values)
    {
        objects.push_back(pdf::PDFObject::createReal(value));
    }
    return pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(objects)));
}

pdf::PDFObjectReference addFormXObject(pdf::PDFDocumentBuilder& builder, const QRectF& rect)
{
    const QByteArray content = QByteArrayLiteral(" ");
    pdf::PDFDictionary dictionary;
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Form"));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("BBox"),
                        arrayOf({ rect.left(), rect.top(), rect.right(), rect.bottom() }));
    // Streams must carry their own /Length: the writer does not add one, and a
    // reader that honours the entry would otherwise fail on the file.
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(content.size()));
    return builder.addObject(pdf::PDFObject::createStream(
        std::make_shared<pdf::PDFStream>(std::move(dictionary), QByteArray(content))));
}

/// One merged field-and-widget dictionary on the page, the shape a real AcroForm
/// uses for its simplest fields.
pdf::PDFObjectReference addMergedField(pdf::PDFDocumentBuilder& builder,
                                       pdf::PDFObjectReference page,
                                       const QByteArray& fieldType,
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
                   arrayOf({ rect.left(), rect.top(), rect.right(), rect.bottom() }));
    field.addEntry(pdf::PDFInplaceOrMemoryString("P"), pdf::PDFObject::createReference(page));
    if (decorate)
    {
        decorate(field);
    }

    const pdf::PDFObjectReference reference =
        builder.addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(field))));

    pdf::PDFObjectFactory pageFactory;
    pageFactory.beginDictionary();
    pageFactory.beginDictionaryItem("Annots");
    pageFactory.beginArray();
    pageFactory << reference;
    pageFactory.endArray();
    pageFactory.endDictionaryItem();
    pageFactory.endDictionary();
    builder.appendTo(page, pageFactory.takeObject());
    return reference;
}

/// A page carrying a text field, a checkbox and a combo box, plus optionally a
/// signature field. The signature field is in the fixture precisely so the
/// "signature fields are excluded" case has something to be excluded from.
pdf::PDFDocument buildFormDocument(bool withSignatureField)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));

    std::vector<pdf::PDFObjectReference> fields;

    fields.push_back(addMergedField(builder, page, "Tx", QStringLiteral("LoopText"), QRectF(10, 10, 90, 30),
                                    [&builder](pdf::PDFDictionary& field)
                                    {
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("V"),
                                                       pdf::PDFObjectFactory::createTextString(QStringLiteral("original")));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("DA"),
                                                       pdf::PDFObject::createString(QByteArrayLiteral("/Helv 0 Tf 0 g")));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("AP"),
                                                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(
                                                           [&builder]()
                                                           {
                                                               pdf::PDFDictionary appearance;
                                                               appearance.addEntry(pdf::PDFInplaceOrMemoryString("N"),
                                                                                   pdf::PDFObject::createReference(addFormXObject(builder, QRectF(10, 10, 90, 30))));
                                                               return appearance;
                                                           }())));
                                    }));

    fields.push_back(addMergedField(builder, page, "Btn", QStringLiteral("LoopCheck"), QRectF(10, 50, 30, 30),
                                    [&builder](pdf::PDFDictionary& field)
                                    {
                                        pdf::PDFDictionary normal;
                                        normal.addEntry(pdf::PDFInplaceOrMemoryString("Yes"),
                                                        pdf::PDFObject::createReference(addFormXObject(builder, QRectF(10, 50, 30, 30))));
                                        normal.addEntry(pdf::PDFInplaceOrMemoryString("Off"),
                                                        pdf::PDFObject::createReference(addFormXObject(builder, QRectF(10, 50, 30, 30))));
                                        pdf::PDFDictionary appearance;
                                        appearance.addEntry(pdf::PDFInplaceOrMemoryString("N"),
                                                            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(normal))));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("AP"),
                                                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(appearance))));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("AS"), pdf::PDFObject::createName("Off"));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("V"), pdf::PDFObject::createName("Off"));
                                    }));

    fields.push_back(addMergedField(builder, page, "Ch", QStringLiteral("LoopChoice"), QRectF(10, 90, 90, 30),
                                    [&builder](pdf::PDFDictionary& field)
                                    {
                                        // Combo flag: the choice admits only its own options.
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("Ff"), pdf::PDFObject::createInteger(1 << 17));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("Opt"),
                                                       pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                                                           pdf::PDFObjectFactory::createTextString(QStringLiteral("Alpha")),
                                                           pdf::PDFObjectFactory::createTextString(QStringLiteral("Beta")) })));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("V"),
                                                       pdf::PDFObjectFactory::createTextString(QStringLiteral("Alpha")));
                                        field.addEntry(pdf::PDFInplaceOrMemoryString("AP"),
                                                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(
                                                           [&builder]()
                                                           {
                                                               pdf::PDFDictionary appearance;
                                                               appearance.addEntry(pdf::PDFInplaceOrMemoryString("N"),
                                                                                   pdf::PDFObject::createReference(addFormXObject(builder, QRectF(10, 90, 90, 30))));
                                                               return appearance;
                                                           }())));
                                    }));

    if (withSignatureField)
    {
        const pdf::PDFObjectReference signature = builder.createSignatureDictionary(
            QByteArrayLiteral("Adobe.PPKLite"),
            QByteArrayLiteral("adbe.pkcs7.detached"),
            QByteArrayLiteral("signed-placeholder"),
            QDateTime::currentDateTimeUtc(),
            0);
        const pdf::PDFObjectReference field = builder.createFormFieldSignature(QStringLiteral("LoopSignature"), {}, signature);
        builder.createInvisibleFormFieldWidget(field, page);
        fields.push_back(field);
    }

    builder.createAcroForm(fields);
    return builder.build();
}

/// An AcroForm whose only content is an XFA packet: no AcroForm fields at all.
pdf::PDFDocument buildXfaOnlyDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));

    pdf::PDFDictionary xfaDictionary;
    const QByteArray xfaPacket = QByteArrayLiteral("<xdp/>");
    xfaDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(xfaPacket.size()));
    const pdf::PDFObjectReference xfa = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(xfaDictionary), QByteArray(xfaPacket))));

    pdf::PDFDictionary form;
    form.addEntry(pdf::PDFInplaceOrMemoryString("Fields"), pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>()));
    form.addEntry(pdf::PDFInplaceOrMemoryString("XFA"), pdf::PDFObject::createReference(xfa));

    pdf::PDFObjectFactory catalogFactory;
    catalogFactory.beginDictionary();
    catalogFactory.beginDictionaryItem("AcroForm");
    catalogFactory << pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(form)));
    catalogFactory.endDictionaryItem();
    catalogFactory.endDictionary();
    builder.mergeTo(builder.getCatalogReference(), catalogFactory.takeObject());
    return builder.build();
}

pdf::PDFDocument readBack(pdf::PDFDocumentPointer document, const QString& path)
{
    pdf::PDFDocumentWriter writer(nullptr);
    if (!writer.write(path, document.data(), true))
    {
        return pdf::PDFDocument();
    }
    pdf::PDFDocumentReader reader(nullptr, nullptr, false, false);
    return reader.readFromFile(path);
}

QString formValue(const pdf::PDFDocument& document, const QString& name, bool* isName)
{
    pdf::PDFFormManager manager(nullptr);
    // The manager parses the document read-only; the const view is what the
    // writer under test handed back.
    manager.setDocument(pdf::PDFModifiedDocument(const_cast<pdf::PDFDocument*>(&document), nullptr, pdf::PDFModifiedDocument::Reset));
    QString value;
    manager.apply([&value, &name, isName](const pdf::PDFFormField* field)
                  {
                      if (value.isEmpty() && field->getName(pdf::PDFFormField::NameType::FullyQualified) == name)
                      {
                          const pdf::PDFObject& object = field->getValue();
                          if (object.isName())
                          {
                              *isName = true;
                              value = QString::fromLatin1(object.getString());
                          }
                          else if (object.isString())
                          {
                              value = pdf::PDFEncoding::convertTextString(object.getString());
                          }
                      } });
    return value;
}

}   // namespace

class DirectEditTest : public QObject
{
    Q_OBJECT

private slots:
    void formFieldsRoundTripThroughTheWriterAndTheFile();
    void signatureFieldIsNeverFilled();
    void xfaOnlyFormIsRefused();
    void unknownFieldAndForeignChoiceValueAreRefused();
    void attachmentRoundTripsThroughTheNameTree();
    void attachmentRemovalUnlistsOnlyTheNamedFile();
    void extractionUsesTheAtomicWriterAndRefusesExecutableAutoOpen();
    void metadataRoundTripsInfoAndXmp();
    void metadataMergesIntoAnExistingPacketAndKeepsForeignProperties();
    void unreadableXmpPacketIsRefusedInsteadOfReplaced();
    void revisionIdentityRoundTripsThroughItsStringForm();
};

void DirectEditTest::formFieldsRoundTripThroughTheWriterAndTheFile()
{
    const pdf::PDFDocument source = buildFormDocument(true);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // Text.
    {
        pdf::FormFieldValueEdit edit;
        edit.field = QStringLiteral("LoopText");
        edit.text = QStringLiteral("Filled by the authoring writer");
        const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
        QVERIFY2(outcome.ok(), pdf::getDirectEditRefusalCode(outcome.refusal));
        QVERIFY(outcome.document);
        QVERIFY(outcome.document.data() != &source);

        const pdf::PDFDocument reloaded = readBack(outcome.document, directory.filePath(QStringLiteral("text.pdf")));
        bool isName = false;
        QCOMPARE(formValue(reloaded, QStringLiteral("LoopText"), &isName), QStringLiteral("Filled by the authoring writer"));
        QVERIFY(!isName);
    }

    // Checkbox: the on-state the widget's appearance declares.
    {
        pdf::FormFieldValueEdit edit;
        edit.field = QStringLiteral("LoopCheck");
        edit.checked = true;
        const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
        QVERIFY2(outcome.ok(), pdf::getDirectEditRefusalCode(outcome.refusal));

        const pdf::PDFDocument reloaded = readBack(outcome.document, directory.filePath(QStringLiteral("check.pdf")));
        bool isName = false;
        QCOMPARE(formValue(reloaded, QStringLiteral("LoopCheck"), &isName), QStringLiteral("Yes"));
        QVERIFY(isName);
    }

    // Choice, within the options the field declares.
    {
        pdf::FormFieldValueEdit edit;
        edit.field = QStringLiteral("LoopChoice");
        edit.text = QStringLiteral("Beta");
        const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
        QVERIFY2(outcome.ok(), pdf::getDirectEditRefusalCode(outcome.refusal));

        const pdf::PDFDocument reloaded = readBack(outcome.document, directory.filePath(QStringLiteral("choice.pdf")));
        bool isName = false;
        QCOMPARE(formValue(reloaded, QStringLiteral("LoopChoice"), &isName), QStringLiteral("Beta"));
    }
}

void DirectEditTest::signatureFieldIsNeverFilled()
{
    const pdf::PDFDocument source = buildFormDocument(true);

    pdf::FormFieldValueEdit edit;
    edit.field = QStringLiteral("LoopSignature");
    edit.text = QStringLiteral("not a signature");
    const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
    QVERIFY(!outcome.ok());
    QCOMPARE(outcome.refusal, pdf::DirectEditRefusal::UnsupportedFieldType);
    QCOMPARE(QString::fromLatin1(pdf::getDirectEditRefusalCode(outcome.refusal)), QStringLiteral("form/unsupported-field-type"));
    QVERIFY(!outcome.document);
}

void DirectEditTest::xfaOnlyFormIsRefused()
{
    const pdf::PDFDocument source = buildXfaOnlyDocument();

    pdf::FormFieldValueEdit edit;
    edit.field = QStringLiteral("AnyField");
    edit.text = QStringLiteral("value");
    const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
    QVERIFY(!outcome.ok());
    QCOMPARE(outcome.refusal, pdf::DirectEditRefusal::XfaUnsupported);
    QCOMPARE(QString::fromLatin1(pdf::getDirectEditRefusalCode(outcome.refusal)), QStringLiteral("form/xfa-unsupported"));
    QVERIFY(!outcome.document);
}

void DirectEditTest::unknownFieldAndForeignChoiceValueAreRefused()
{
    const pdf::PDFDocument source = buildFormDocument(false);

    {
        pdf::FormFieldValueEdit edit;
        edit.field = QStringLiteral("NoSuchField");
        edit.text = QStringLiteral("value");
        const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
        QVERIFY(!outcome.ok());
        QCOMPARE(outcome.refusal, pdf::DirectEditRefusal::FieldNotFound);
    }

    {
        pdf::FormFieldValueEdit edit;
        edit.field = QStringLiteral("LoopChoice");
        edit.text = QStringLiteral("Gamma");
        const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
        QVERIFY(!outcome.ok());
        QCOMPARE(outcome.refusal, pdf::DirectEditRefusal::ValueNotAllowed);
    }

    {
        // A button edit with neither shape, and a text edit carrying a checkbox
        // shape, are both ill-formed rather than guessed.
        pdf::FormFieldValueEdit edit;
        edit.field = QStringLiteral("LoopCheck");
        const pdf::DirectEditOutcome outcome = pdf::PDFFormValueWriter::setFieldValue(&source, edit);
        QVERIFY(!outcome.ok());
        QCOMPARE(outcome.refusal, pdf::DirectEditRefusal::InvalidRequest);
    }
}

void DirectEditTest::attachmentRoundTripsThroughTheNameTree()
{
    const pdf::PDFDocument source = buildFormDocument(false);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(source.getCatalog()->getEmbeddedFiles().empty());

    pdf::AttachmentEdit edit;
    edit.name = QStringLiteral("notes.txt");
    edit.description = QStringLiteral("Reviewer notes");
    edit.mimeType = QByteArrayLiteral("text/plain");
    edit.contents = QByteArrayLiteral("attached payload");

    const pdf::DirectEditOutcome outcome = pdf::PDFAttachmentWriter::add(&source, edit);
    QVERIFY2(outcome.ok(), pdf::getDirectEditRefusalCode(outcome.refusal));

    const pdf::PDFDocument reloaded = readBack(outcome.document, directory.filePath(QStringLiteral("attachment.pdf")));
    const auto& files = reloaded.getCatalog()->getEmbeddedFiles();
    QCOMPARE(files.size(), size_t(1));
    const auto item = files.find(QByteArrayLiteral("notes.txt"));
    QVERIFY(item != files.cend());
    QCOMPARE(item->second.getPlatformFileName(), QStringLiteral("notes.txt"));
    QCOMPARE(item->second.getDescription(), QStringLiteral("Reviewer notes"));
    const pdf::PDFEmbeddedFile* file = item->second.getPlatformFile();
    QVERIFY(file);
    QVERIFY(file->isValid());
    QCOMPARE(*file->getStream()->getContent(), QByteArrayLiteral("attached payload"));

    // A second attachment under the same name is refused, and the name is
    // carried in the file specification's unicode entry.
    const pdf::DirectEditOutcome duplicate = pdf::PDFAttachmentWriter::add(outcome.document.data(), edit);
    QVERIFY(!duplicate.ok());
    QCOMPARE(duplicate.refusal, pdf::DirectEditRefusal::AttachmentNameTaken);

    pdf::AttachmentEdit escaping;
    escaping.name = QStringLiteral("../escape.txt");
    escaping.contents = QByteArrayLiteral("x");
    const pdf::DirectEditOutcome refusedName = pdf::PDFAttachmentWriter::add(&source, escaping);
    QVERIFY(!refusedName.ok());
    QCOMPARE(refusedName.refusal, pdf::DirectEditRefusal::AttachmentNameInvalid);
}

void DirectEditTest::attachmentRemovalUnlistsOnlyTheNamedFile()
{
    const pdf::PDFDocument source = buildFormDocument(false);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::AttachmentEdit first;
    first.name = QStringLiteral("keep.txt");
    first.contents = QByteArrayLiteral("keep");
    pdf::AttachmentEdit second;
    second.name = QStringLiteral("drop.txt");
    second.contents = QByteArrayLiteral("drop");

    const pdf::DirectEditOutcome withFirst = pdf::PDFAttachmentWriter::add(&source, first);
    QVERIFY(withFirst.ok());
    const pdf::DirectEditOutcome withBoth = pdf::PDFAttachmentWriter::add(withFirst.document.data(), second);
    QVERIFY(withBoth.ok());
    QCOMPARE(withBoth.document->getCatalog()->getEmbeddedFiles().size(), size_t(2));

    const pdf::DirectEditOutcome removed = pdf::PDFAttachmentWriter::remove(withBoth.document.data(), QStringLiteral("drop.txt"));
    QVERIFY2(removed.ok(), pdf::getDirectEditRefusalCode(removed.refusal));

    const pdf::PDFDocument reloaded = readBack(removed.document, directory.filePath(QStringLiteral("removed.pdf")));
    const auto& files = reloaded.getCatalog()->getEmbeddedFiles();
    QCOMPARE(files.size(), size_t(1));
    QVERIFY(files.find(QByteArrayLiteral("keep.txt")) != files.cend());

    // Removing it again is a typed refusal, not a silent success.
    const pdf::DirectEditOutcome missing = pdf::PDFAttachmentWriter::remove(removed.document.data(), QStringLiteral("drop.txt"));
    QVERIFY(!missing.ok());
    QCOMPARE(missing.refusal, pdf::DirectEditRefusal::AttachmentNotFound);

    // And removing the last one leaves no EmbeddedFiles entry behind.
    const pdf::DirectEditOutcome emptied = pdf::PDFAttachmentWriter::remove(removed.document.data(), QStringLiteral("keep.txt"));
    QVERIFY(emptied.ok());
    QVERIFY(emptied.document->getCatalog()->getEmbeddedFiles().empty());
}

void DirectEditTest::extractionUsesTheAtomicWriterAndRefusesExecutableAutoOpen()
{
    const pdf::PDFDocument source = buildFormDocument(false);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::AttachmentEdit text;
    text.name = QStringLiteral("readme.txt");
    text.contents = QByteArrayLiteral("extracted text payload");
    const pdf::DirectEditOutcome withText = pdf::PDFAttachmentWriter::add(&source, text);
    QVERIFY(withText.ok());

    const pdf::PDFAttachmentWriter::ExtractionOutcome extracted =
        pdf::PDFAttachmentWriter::extractToDirectory(withText.document.data(), QStringLiteral("readme.txt"), directory.path());
    QVERIFY2(extracted.written, qPrintable(extracted.typedError));
    QVERIFY(!extracted.autoOpenRefused);
    QCOMPARE(extracted.path, directory.filePath(QStringLiteral("readme.txt")));

    QFile written(extracted.path);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), QByteArrayLiteral("extracted text payload"));
    written.close();

    // The atomic writer refuses to replace an existing file, so a second
    // extraction of the same name is a typed refusal and the file is untouched.
    const pdf::PDFAttachmentWriter::ExtractionOutcome again =
        pdf::PDFAttachmentWriter::extractToDirectory(withText.document.data(), QStringLiteral("readme.txt"), directory.path());
    QVERIFY(!again.written);
    QCOMPARE(again.refusal, pdf::DirectEditRefusal::WriteRefused);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), QByteArrayLiteral("extracted text payload"));

    // An executable-type attachment extracts, but must not be auto-opened.
    pdf::AttachmentEdit executable;
    executable.name = QStringLiteral("setup.exe");
    executable.contents = QByteArrayLiteral("MZ not really an executable");
    const pdf::DirectEditOutcome withExecutable = pdf::PDFAttachmentWriter::add(&source, executable);
    QVERIFY(withExecutable.ok());
    QVERIFY(pdf::PDFAttachmentWriter::isExecutableFileName(QStringLiteral("setup.exe")));
    QVERIFY(pdf::PDFAttachmentWriter::isExecutableFileName(QStringLiteral("SCRIPT.PS1")));
    QVERIFY(!pdf::PDFAttachmentWriter::isExecutableFileName(QStringLiteral("readme.txt")));

    const pdf::PDFAttachmentWriter::ExtractionOutcome executableOutcome =
        pdf::PDFAttachmentWriter::extractToDirectory(withExecutable.document.data(), QStringLiteral("setup.exe"), directory.path());
    QVERIFY2(executableOutcome.written, qPrintable(executableOutcome.typedError));
    QVERIFY(executableOutcome.autoOpenRefused);

    // An unknown name is a refusal with the stable code.
    const pdf::PDFAttachmentWriter::ExtractionOutcome missing =
        pdf::PDFAttachmentWriter::extractToDirectory(withText.document.data(), QStringLiteral("absent.txt"), directory.path());
    QVERIFY(!missing.written);
    QCOMPARE(missing.refusal, pdf::DirectEditRefusal::AttachmentNotFound);
    QCOMPARE(missing.typedError, QStringLiteral("attachment/not-found"));
}

void DirectEditTest::metadataRoundTripsInfoAndXmp()
{
    const pdf::PDFDocument source = buildFormDocument(false);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(pdf::PDFMetadataWriter::readXmpPacket(&source).isEmpty());

    pdf::DocumentMetadataEdit edit;
    edit.title = QStringLiteral("Loop title");
    edit.author = QStringLiteral("Loop author");
    edit.subject = QStringLiteral("Loop subject");
    edit.keywords = QStringLiteral("loop, metadata");
    edit.creator = QStringLiteral("LoopEditor");

    const pdf::DirectEditOutcome outcome = pdf::PDFMetadataWriter::update(&source, edit);
    QVERIFY2(outcome.ok(), pdf::getDirectEditRefusalCode(outcome.refusal));

    const pdf::PDFDocument reloaded = readBack(outcome.document, directory.filePath(QStringLiteral("metadata.pdf")));
    QCOMPARE(reloaded.getInfo()->title, QStringLiteral("Loop title"));
    QCOMPARE(reloaded.getInfo()->author, QStringLiteral("Loop author"));
    QCOMPARE(reloaded.getInfo()->subject, QStringLiteral("Loop subject"));
    QCOMPARE(reloaded.getInfo()->keywords, QStringLiteral("loop, metadata"));
    QCOMPARE(reloaded.getInfo()->creator, QStringLiteral("LoopEditor"));

    const QByteArray packet = pdf::PDFMetadataWriter::readXmpPacket(&reloaded);
    QVERIFY(!packet.isEmpty());
    QVERIFY(packet.contains("dc:title"));
    QVERIFY(packet.contains("Loop title"));
    QVERIFY(packet.contains("dc:creator"));
    QVERIFY(packet.contains("Loop author"));
    QVERIFY(packet.contains("dc:description"));
    QVERIFY(packet.contains("Loop subject"));
    QVERIFY(packet.contains("pdf:Keywords"));
    QVERIFY(packet.contains("loop, metadata"));
    QVERIFY(packet.contains("xmp:CreatorTool"));
    QVERIFY(packet.contains("LoopEditor"));

    // A second edit that only names one field keeps the others.
    pdf::DocumentMetadataEdit followUp;
    followUp.title = QStringLiteral("Second title");
    const pdf::DirectEditOutcome second = pdf::PDFMetadataWriter::update(outcome.document.data(), followUp);
    QVERIFY2(second.ok(), pdf::getDirectEditRefusalCode(second.refusal));
    QCOMPARE(second.document->getInfo()->title, QStringLiteral("Second title"));
    QCOMPARE(second.document->getInfo()->author, QStringLiteral("Loop author"));
    const QByteArray mergedPacket = pdf::PDFMetadataWriter::readXmpPacket(second.document.data());
    QVERIFY(mergedPacket.contains("Second title"));
    QVERIFY(mergedPacket.contains("Loop author"));
}

void DirectEditTest::metadataMergesIntoAnExistingPacketAndKeepsForeignProperties()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    // A packet that carries a conformance claim and another tool's property.
    builder.setCatalogMetadata(QByteArrayLiteral(
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\" xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\" pdfaid:part=\"2\" pdfaid:conformance=\"B\">\n"
        "   <dc:title xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><rdf:Alt><rdf:li xml:lang=\"x-default\">Old title</rdf:li></rdf:Alt></dc:title>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>"));
    const pdf::PDFDocument source = builder.build();
    QVERIFY(!pdf::PDFMetadataWriter::readXmpPacket(&source).isEmpty());

    pdf::DocumentMetadataEdit edit;
    edit.title = QStringLiteral("New title");
    const pdf::DirectEditOutcome outcome = pdf::PDFMetadataWriter::update(&source, edit);
    QVERIFY2(outcome.ok(), pdf::getDirectEditRefusalCode(outcome.refusal));

    const QByteArray packet = pdf::PDFMetadataWriter::readXmpPacket(outcome.document.data());
    // The property this edit owns is replaced, exactly once, and the packet is
    // still a packet rather than an XML document with a declaration bolted on.
    QVERIFY(packet.contains("<dc:title>"));
    QVERIFY(packet.contains("New title"));
    QVERIFY(!packet.contains("Old title"));
    QVERIFY(!packet.startsWith("<?xml"));
    QCOMPARE(packet.count("<dc:title>"), 1);
    // Everything else the packet carried survives, including the PDF/A claim.
    QVERIFY(packet.contains("pdfaid:part"));
    QVERIFY(packet.contains("pdfaid:conformance"));
    QVERIFY(packet.contains("xpacket"));
}

void DirectEditTest::unreadableXmpPacketIsRefusedInsteadOfReplaced()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setCatalogMetadata(QByteArrayLiteral("<x:xmpmeta><rdf:RDF> this is not well formed"));
    const pdf::PDFDocument source = builder.build();

    pdf::DocumentMetadataEdit edit;
    edit.title = QStringLiteral("Never written");
    const pdf::DirectEditOutcome outcome = pdf::PDFMetadataWriter::update(&source, edit);
    QVERIFY(!outcome.ok());
    QCOMPARE(outcome.refusal, pdf::DirectEditRefusal::XmpUnreadable);
    QCOMPARE(QString::fromLatin1(pdf::getDirectEditRefusalCode(outcome.refusal)), QStringLiteral("metadata/xmp-unreadable"));
    QVERIFY(!outcome.document);
    // The packet is untouched: it is still the unreadable one.
    QCOMPARE(pdf::PDFMetadataWriter::readXmpPacket(&source),
             QByteArrayLiteral("<x:xmpmeta><rdf:RDF> this is not well formed"));
}

void DirectEditTest::revisionIdentityRoundTripsThroughItsStringForm()
{
    pdf::PDFRevisionIdentity identity;
    identity.document.sourceDataHash = QByteArray::fromHex("00ff10");
    identity.document.documentId = QStringLiteral("1a2b");
    identity.documentRevision = 7;
    identity.cacheGeneration = 9;
    identity.effectiveProfileIdentity = QStringLiteral("loop-default:1");

    const pdf::PDFRevisionIdentity parsed = pdf::PDFRevisionIdentity::fromString(identity.toString());
    QVERIFY(parsed.isValid());
    QVERIFY(parsed == identity);

    // A malformed value yields an invalid identity rather than a guessed one.
    QVERIFY(!pdf::PDFRevisionIdentity::fromString(QString()).isValid());
    QVERIFY(!pdf::PDFRevisionIdentity::fromString(QStringLiteral("not-a-revision")).isValid());
    QVERIFY(!pdf::PDFRevisionIdentity::fromString(QStringLiteral("00ff:1a2b:notanumber:9:")).isValid());
}

QTEST_MAIN(DirectEditTest)

#include "tst_directedittest.moc"
