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

#include "pdfdirectedit.h"

#include "pdfcatalog.h"
#include "pdfdocumentbuilder.h"
#include "pdffile.h"
#include "pdffilenamesanitizer.h"
#include "pdfform.h"
#include "pdfsafefilewriter.h"

#include <QDir>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pdf
{

namespace
{

constexpr const char* NamespaceDublinCore = "http://purl.org/dc/elements/1.1/";
constexpr const char* NamespaceRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr const char* NamespacePdf = "http://ns.adobe.com/pdf/1.3/";
constexpr const char* NamespaceXmp = "http://ns.adobe.com/xap/1.0/";

enum class XmpValueShape
{
    Text,   ///< A plain text element.
    LocalizedAlt,   ///< An rdf:Alt with one x-default rdf:li (dc:title, dc:description).
    Sequence   ///< An rdf:Seq with one rdf:li (dc:creator).
};

/// One XMP property this module owns. Matching accepts the conventional prefix
/// and, when the reader exposes namespaces, the namespace URI as well.
struct XmpProperty
{
    QByteArray qualifiedName;
    QByteArray namespaceUri;
    XmpValueShape shape = XmpValueShape::Text;
    std::optional<QString> value;
    bool written = false;
    int elementOccurrences = 0;   ///< Matched as an element inside rdf:Description.
    int attributeOccurrences = 0;   ///< Matched as an attribute on rdf:Description.
};

bool isRdfDescription(const QXmlStreamReader& reader)
{
    return reader.name() == QLatin1String("Description") &&
           (reader.qualifiedName() == QLatin1String("rdf:Description") ||
            reader.namespaceUri() == QLatin1String(NamespaceRdf));
}

bool matchesProperty(const QXmlStreamReader& reader, const XmpProperty& property)
{
    if (reader.qualifiedName() == QLatin1String(property.qualifiedName))
    {
        return true;
    }

    // Namespace-aware fallback: a packet may bind another prefix to the same
    // namespace. Only used when the reader actually reports a namespace.
    const QString uri = reader.namespaceUri().toString();
    if (uri.isEmpty() || uri != QLatin1String(property.namespaceUri))
    {
        return false;
    }
    const QByteArray localName = property.qualifiedName.mid(property.qualifiedName.indexOf(':') + 1);
    return reader.name() == QString::fromLatin1(localName);
}

/// The attribute form of `matchesProperty`: a property carried as an attribute
/// on rdf:Description (the common attribute shorthand) matches by qualified
/// name first and by namespace URI plus local name second.
bool matchesPropertyAttribute(const QXmlStreamAttribute& attribute, const XmpProperty& property)
{
    if (attribute.qualifiedName() == QLatin1String(property.qualifiedName))
    {
        return true;
    }

    const QString uri = attribute.namespaceUri().toString();
    if (uri.isEmpty() || uri != QLatin1String(property.namespaceUri))
    {
        return false;
    }
    const QByteArray localName = property.qualifiedName.mid(property.qualifiedName.indexOf(':') + 1);
    return attribute.name() == QString::fromLatin1(localName);
}

/// Records how each property occurs in the packet: as an element inside an
/// rdf:Description and as an attribute on one. The carried value is whether the
/// packet has at least one rdf:Description; `std::nullopt` means it did not
/// parse.
std::optional<bool> recordXmpOccurrences(const QByteArray& existing, std::vector<XmpProperty>& properties)
{
    QXmlStreamReader reader(existing);
    reader.setNamespaceProcessing(true);

    int depth = 0;
    int descriptionDepth = -1;
    bool descriptionSeen = false;

    while (!reader.atEnd())
    {
        reader.readNext();
        if (reader.hasError())
        {
            return std::nullopt;
        }

        switch (reader.tokenType())
        {
            case QXmlStreamReader::StartElement:
            {
                ++depth;
                if (isRdfDescription(reader))
                {
                    descriptionSeen = true;
                    descriptionDepth = depth;
                    for (const QXmlStreamAttribute& attribute : reader.attributes())
                    {
                        for (XmpProperty& property : properties)
                        {
                            if (matchesPropertyAttribute(attribute, property))
                            {
                                ++property.attributeOccurrences;
                            }
                        }
                    }
                }
                else if (descriptionDepth >= 0)
                {
                    for (XmpProperty& property : properties)
                    {
                        if (matchesProperty(reader, property))
                        {
                            ++property.elementOccurrences;
                        }
                    }
                }
                break;
            }

            case QXmlStreamReader::EndElement:
                --depth;
                if (depth < descriptionDepth)
                {
                    descriptionDepth = -1;
                }
                break;

            default:
                break;
        }
    }

    if (reader.hasError())
    {
        return std::nullopt;
    }
    return descriptionSeen;
}

void writeXmpProperty(QXmlStreamWriter& writer, const XmpProperty& property, bool declareNamespace = false)
{
    // The qualified-name write path bypasses the writer's namespace handling:
    // neither the packet being merged into nor the fresh packet necessarily
    // declares the prefix at the insertion point, so a merged property declares
    // the namespace on the element itself.
    const QByteArray qualifiedName = property.qualifiedName;
    const QString value = property.value.value_or(QString());

    writer.writeStartElement(QString::fromLatin1(qualifiedName));
    if (declareNamespace)
    {
        const QByteArray prefix = qualifiedName.left(qualifiedName.indexOf(':'));
        writer.writeNamespace(QString::fromLatin1(property.namespaceUri), QString::fromLatin1(prefix));
    }
    switch (property.shape)
    {
        case XmpValueShape::LocalizedAlt:
        {
            writer.writeStartElement(QStringLiteral("rdf:Alt"));
            writer.writeStartElement(QStringLiteral("rdf:li"));
            writer.writeAttribute(QStringLiteral("xml:lang"), QStringLiteral("x-default"));
            writer.writeCharacters(value);
            writer.writeEndElement();
            writer.writeEndElement();
            break;
        }

        case XmpValueShape::Sequence:
        {
            writer.writeStartElement(QStringLiteral("rdf:Seq"));
            writer.writeStartElement(QStringLiteral("rdf:li"));
            writer.writeCharacters(value);
            writer.writeEndElement();
            writer.writeEndElement();
            break;
        }

        case XmpValueShape::Text:
        default:
            writer.writeCharacters(value);
            break;
    }
    writer.writeEndElement();
}

std::vector<XmpProperty> propertiesFor(const DocumentMetadataEdit& edit)
{
    std::vector<XmpProperty> properties;
    properties.push_back({ QByteArrayLiteral("dc:title"), QByteArray(NamespaceDublinCore), XmpValueShape::LocalizedAlt, edit.title, false });
    properties.push_back({ QByteArrayLiteral("dc:creator"), QByteArray(NamespaceDublinCore), XmpValueShape::Sequence, edit.author, false });
    properties.push_back({ QByteArrayLiteral("dc:description"), QByteArray(NamespaceDublinCore), XmpValueShape::LocalizedAlt, edit.subject, false });
    properties.push_back({ QByteArrayLiteral("pdf:Keywords"), QByteArray(NamespacePdf), XmpValueShape::Text, edit.keywords, false });
    properties.push_back({ QByteArrayLiteral("xmp:CreatorTool"), QByteArray(NamespaceXmp), XmpValueShape::Text, edit.creator, false });
    return properties;
}

/// A packet for a document that carries none. Uses the conventional prefixes so
/// the result reads like every other tool's packet.
QByteArray freshXmpPacket(const DocumentMetadataEdit& edit)
{
    const std::vector<XmpProperty> properties = propertiesFor(edit);

    QByteArray output;
    QXmlStreamWriter writer(&output);
    writer.writeProcessingInstruction(QStringLiteral("xpacket"),
                                      QStringLiteral("begin=\"\uFEFF\" id=\"W5M0MpCehiHzreSzNTczkc9d\""));
    writer.writeStartElement(QStringLiteral("x:xmpmeta"));
    writer.writeNamespace(QLatin1String("adobe:ns:meta/"), QStringLiteral("x"));
    writer.writeStartElement(QStringLiteral("rdf:RDF"));
    writer.writeNamespace(QLatin1String(NamespaceRdf), QStringLiteral("rdf"));
    writer.writeStartElement(QStringLiteral("rdf:Description"));
    writer.writeAttribute(QStringLiteral("rdf:about"), QString());
    writer.writeNamespace(QLatin1String(NamespaceDublinCore), QStringLiteral("dc"));
    writer.writeNamespace(QLatin1String(NamespacePdf), QStringLiteral("pdf"));
    writer.writeNamespace(QLatin1String(NamespaceXmp), QStringLiteral("xmp"));

    for (const XmpProperty& property : properties)
    {
        if (property.value)
        {
            writeXmpProperty(writer, property);
        }
    }

    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeProcessingInstruction(QStringLiteral("xpacket"), QStringLiteral("end=\"w\""));
    return output;
}

/// Writes an rdf:Description start element token by token when it carries an
/// attribute this edit merges into: the substituted value for a matching
/// property, the original attribute otherwise. Returns false when the element
/// carries no such attribute, in which case the caller copies the token
/// instead.
bool writeStartElementWithMergedAttributes(QXmlStreamWriter& writer,
                                           const QXmlStreamReader& reader,
                                           std::vector<XmpProperty>& properties)
{
    bool mergesAttribute = false;
    for (const XmpProperty& property : properties)
    {
        if (property.value && !property.written && property.attributeOccurrences == 1)
        {
            for (const QXmlStreamAttribute& attribute : reader.attributes())
            {
                mergesAttribute = mergesAttribute || matchesPropertyAttribute(attribute, property);
            }
        }
    }
    if (!mergesAttribute)
    {
        return false;
    }

    writer.writeStartElement(reader.qualifiedName().toString());
    for (const QXmlStreamNamespaceDeclaration& declaration : reader.namespaceDeclarations())
    {
        if (declaration.prefix().isEmpty())
        {
            // writeNamespace() would invent a prefix for a default declaration.
            writer.writeDefaultNamespace(declaration.namespaceUri().toString());
        }
        else
        {
            writer.writeNamespace(declaration.namespaceUri().toString(), declaration.prefix().toString());
        }
    }

    for (const QXmlStreamAttribute& attribute : reader.attributes())
    {
        XmpProperty* merged = nullptr;
        for (XmpProperty& property : properties)
        {
            if (property.value && !property.written && property.attributeOccurrences == 1 &&
                matchesPropertyAttribute(attribute, property))
            {
                merged = &property;
                break;
            }
        }

        if (merged)
        {
            merged->written = true;
            writer.writeAttribute(attribute.qualifiedName(), *merged->value);
        }
        else
        {
            writer.writeAttribute(attribute.qualifiedName(), attribute.value());
        }
    }
    return true;
}

/// Merges the edit's properties into an existing packet, copying every token it
/// does not own. `std::nullopt` means the packet did not parse, or carries a
/// property this edit writes more than once and so cannot be merged
/// unambiguously, and the caller refuses rather than overwriting a packet it
/// could not read.
std::optional<QByteArray> mergeXmpPacket(const QByteArray& existing, const DocumentMetadataEdit& edit)
{
    std::vector<XmpProperty> properties = propertiesFor(edit);

    // A property carried in a later sibling rdf:Description, or as an
    // attribute on one, is still the property; it has to be replaced where it
    // is, because appending a second copy beside it would leave the packet
    // carrying an old and a new value.
    const std::optional<bool> descriptionSeen = recordXmpOccurrences(existing, properties);
    if (!descriptionSeen)
    {
        return std::nullopt;
    }
    for (const XmpProperty& property : properties)
    {
        if (property.value && property.elementOccurrences + property.attributeOccurrences > 1)
        {
            return std::nullopt;
        }
    }
    if (!*descriptionSeen)
    {
        // A packet with no rdf:Description carries no XMP properties to
        // preserve, so a standard one is written instead.
        return freshXmpPacket(edit);
    }

    QXmlStreamReader reader(existing);
    reader.setNamespaceProcessing(true);
    QByteArray output;
    QXmlStreamWriter writer(&output);

    int depth = 0;
    int descriptionDepth = -1;
    bool insertedMissing = false;

    while (!reader.atEnd())
    {
        reader.readNext();
        if (reader.hasError())
        {
            return std::nullopt;
        }

        switch (reader.tokenType())
        {
            case QXmlStreamReader::StartDocument:
                // The packet is not a whole XML document: an XML declaration
                // before the xpacket wrapper would make it invalid XMP.
                break;

            case QXmlStreamReader::StartElement:
            {
                ++depth;
                if (isRdfDescription(reader))
                {
                    descriptionDepth = depth;
                    if (writeStartElementWithMergedAttributes(writer, reader, properties))
                    {
                        break;
                    }
                }
                else if (descriptionDepth >= 0)
                {
                    bool replaced = false;
                    for (XmpProperty& property : properties)
                    {
                        if (property.value && !property.written && matchesProperty(reader, property))
                        {
                            property.written = true;
                            // The replaced element may be the only place its
                            // prefix was declared (a child-declared binding), so
                            // the replacement declares the namespace as well.
                            writeXmpProperty(writer, property, true);
                            replaced = true;
                            break;
                        }
                    }
                    if (replaced)
                    {
                        // skipCurrentElement consumes the element up to and
                        // including its EndElement, so no token is copied and
                        // the depth returns to the enclosing element.
                        reader.skipCurrentElement();
                        --depth;
                        continue;
                    }
                }
                writer.writeCurrentToken(reader);
                break;
            }

            case QXmlStreamReader::EndElement:
            {
                if (descriptionDepth == depth && !insertedMissing)
                {
                    insertedMissing = true;
                    for (XmpProperty& property : properties)
                    {
                        if (property.value && !property.written && property.elementOccurrences == 0 &&
                            property.attributeOccurrences == 0)
                        {
                            property.written = true;
                            writeXmpProperty(writer, property, true);
                        }
                    }
                }
                --depth;
                if (depth < descriptionDepth)
                {
                    descriptionDepth = -1;
                }
                writer.writeCurrentToken(reader);
                break;
            }

            default:
                writer.writeCurrentToken(reader);
                break;
        }
    }

    if (reader.hasError())
    {
        return std::nullopt;
    }
    return output;
}

/// Finds the embedded file specification for a name, matching the raw name-tree
/// key first and the decoded platform file name second.
const PDFFileSpecification* findSpecification(const PDFDocument* document, const QString& name)
{
    if (!document || !document->getCatalog())
    {
        return nullptr;
    }

    const QByteArray utf8 = name.toUtf8();
    for (const auto& item : document->getCatalog()->getEmbeddedFiles())
    {
        if (item.first == utf8 || item.second.getPlatformFileName() == name)
        {
            return &item.second;
        }
    }
    return nullptr;
}

std::optional<QByteArray> findEmbeddedFileKey(const PDFDocument* document, const QString& name)
{
    if (!document || !document->getCatalog())
    {
        return std::nullopt;
    }

    const QByteArray utf8 = name.toUtf8();
    for (const auto& item : document->getCatalog()->getEmbeddedFiles())
    {
        if (item.first == utf8 || item.second.getPlatformFileName() == name)
        {
            return item.first;
        }
    }
    return std::nullopt;
}

/// Rewrites the catalog /Names dictionary with the given EmbeddedFiles entries.
/// Every other entry (Dests, JavaScript, ...) is carried over verbatim; an empty
/// entry set removes the EmbeddedFiles key, and an empty Names dictionary
/// removes the key itself.
void writeEmbeddedFileNamesTree(PDFDocumentBuilder* builder,
                                const std::map<QByteArray, PDFObjectReference>& entries)
{
    const PDFObjectStorage* storage = builder->getStorage();
    PDFDictionary names;
    if (const PDFDictionary* catalogDictionary =
            storage->getDictionaryFromObject(PDFObject::createReference(builder->getCatalogReference())))
    {
        if (const PDFDictionary* existing = storage->getDictionaryFromObject(catalogDictionary->get("Names")))
        {
            for (size_t i = 0; i < existing->getCount(); ++i)
            {
                if (existing->getKey(i) != "EmbeddedFiles")
                {
                    names.addEntry(existing->getKey(i), PDFObject(existing->getValue(i)));
                }
            }
        }
    }

    if (!entries.empty())
    {
        PDFObjectFactory treeFactory;
        treeFactory.beginDictionary();
        treeFactory.beginDictionaryItem("Names");
        treeFactory.beginArray();
        for (const auto& entry : entries)
        {
            treeFactory << WrapString(entry.first);
            treeFactory << entry.second;
        }
        treeFactory.endArray();
        treeFactory.endDictionaryItem();
        treeFactory.endDictionary();
        names.addEntry(PDFInplaceOrMemoryString("EmbeddedFiles"),
                       PDFObject::createReference(builder->addObject(treeFactory.takeObject())));
    }

    PDFObjectFactory catalogFactory;
    catalogFactory.beginDictionary();
    catalogFactory.beginDictionaryItem("Names");
    if (names.getCount() > 0)
    {
        catalogFactory << PDFObject::createDictionary(std::make_shared<PDFDictionary>(std::move(names)));
    }
    else
    {
        catalogFactory << nullptr;
    }
    catalogFactory.endDictionaryItem();
    catalogFactory.endDictionary();
    builder->mergeTo(builder->getCatalogReference(), catalogFactory.takeObject());
}

}   // namespace

const char* getDirectEditRefusalCode(DirectEditRefusal refusal)
{
    switch (refusal)
    {
        case DirectEditRefusal::None:
            return "";
        case DirectEditRefusal::InvalidRequest:
            return "direct-edit/invalid-request";
        case DirectEditRefusal::NoDocument:
            return "direct-edit/no-document";
        case DirectEditRefusal::NoForm:
            return "form/not-present";
        case DirectEditRefusal::XfaUnsupported:
            return "form/xfa-unsupported";
        case DirectEditRefusal::FieldNotFound:
            return "form/field-not-found";
        case DirectEditRefusal::FieldNotEditable:
            return "form/field-not-editable";
        case DirectEditRefusal::UnsupportedFieldType:
            return "form/unsupported-field-type";
        case DirectEditRefusal::ValueNotAllowed:
            return "form/value-not-allowed";
        case DirectEditRefusal::ValueUnchanged:
            return "direct-edit/value-unchanged";
        case DirectEditRefusal::AttachmentNotFound:
            return "attachment/not-found";
        case DirectEditRefusal::AttachmentNameTaken:
            return "attachment/name-taken";
        case DirectEditRefusal::AttachmentNameInvalid:
            return "attachment/name-invalid";
        case DirectEditRefusal::XmpUnreadable:
            return "metadata/xmp-unreadable";
        case DirectEditRefusal::MetadataUnchanged:
            return "metadata/unchanged";
        case DirectEditRefusal::WriteRefused:
            return "attachment/output-write-refused";
        case DirectEditRefusal::AttachmentSpecificationNotAddressable:
            return "attachment/specification-not-addressable";
    }

    return "direct-edit/invalid-request";
}

DirectEditOutcome PDFFormValueWriter::setFieldValue(const PDFDocument* document, const FormFieldValueEdit& edit)
{
    DirectEditOutcome outcome;
    if (!document)
    {
        outcome.refusal = DirectEditRefusal::NoDocument;
        return outcome;
    }
    if (edit.field.isEmpty() || (!edit.text.has_value() && !edit.checked.has_value()))
    {
        outcome.refusal = DirectEditRefusal::InvalidRequest;
        return outcome;
    }

    // The form manager owns the parsed field model; the modifier owns the copy
    // the edit is written into. Both follow the interactive editor's own path,
    // so widget appearances are updated by the same code. The manager reads the
    // document and writes only into the modifier's builder, so the const view
    // the caller handed in is not mutated.
    PDFFormManager formManager(nullptr);
    formManager.setDocument(PDFModifiedDocument(const_cast<PDFDocument*>(document), nullptr, PDFModifiedDocument::Reset));

    const PDFForm* form = formManager.getForm();
    if (!form || form->getFormType() == PDFForm::FormType::None)
    {
        outcome.refusal = DirectEditRefusal::NoForm;
        return outcome;
    }
    if (form->isXFAForm())
    {
        // The values an XFA form presents live in its packet. Writing only the
        // AcroForm mirror would leave the two disagreeing, so it is refused.
        outcome.refusal = DirectEditRefusal::XfaUnsupported;
        return outcome;
    }

    PDFFormField* field = nullptr;
    PDFFormField* firstMatch = nullptr;
    formManager.modify([&field, &firstMatch, &edit](PDFFormField* candidate)
                       {
                           if (candidate->getName(PDFFormField::NameType::FullyQualified) != edit.field)
                           {
                               return;
                           }

                           // Same-named fields are one field per PDF semantics.
                           // A widget-less parent may match before the editable
                           // terminal field, so the match carrying widgets wins.
                           if (!firstMatch)
                           {
                               firstMatch = candidate;
                           }
                           if (!field && !candidate->getWidgets().empty())
                           {
                               field = candidate;
                           } });
    if (!field)
    {
        field = firstMatch;
    }
    if (!field)
    {
        outcome.refusal = DirectEditRefusal::FieldNotFound;
        return outcome;
    }

    PDFObject value;
    switch (field->getFieldType())
    {
        case PDFFormField::FieldType::Text:
        {
            if (!edit.text)
            {
                outcome.refusal = DirectEditRefusal::InvalidRequest;
                return outcome;
            }
            value = PDFObjectFactory::createTextString(*edit.text);
            break;
        }

        case PDFFormField::FieldType::Choice:
        {
            if (!edit.text)
            {
                outcome.refusal = DirectEditRefusal::InvalidRequest;
                return outcome;
            }
            const auto* choice = static_cast<const PDFFormFieldChoice*>(field);
            if (!choice->isEditableComboBox())
            {
                bool known = false;
                for (const PDFFormFieldChoice::Option& option : choice->getOptions())
                {
                    known = known || option.exportString == *edit.text || option.userString == *edit.text;
                }
                if (!known)
                {
                    outcome.refusal = DirectEditRefusal::ValueNotAllowed;
                    return outcome;
                }
            }
            value = PDFObjectFactory::createTextString(*edit.text);
            break;
        }

        case PDFFormField::FieldType::Button:
        {
            if (!edit.checked)
            {
                outcome.refusal = DirectEditRefusal::InvalidRequest;
                return outcome;
            }
            const auto* button = static_cast<const PDFFormFieldButton*>(field);
            if (button->getButtonType() == PDFFormFieldButton::ButtonType::PushButton || button->getWidgets().empty())
            {
                outcome.refusal = DirectEditRefusal::FieldNotEditable;
                return outcome;
            }
            const QByteArray onState = PDFFormFieldButton::getOnAppearanceState(&formManager, &button->getWidgets().front());
            if (*edit.checked && onState.isEmpty())
            {
                // A button whose appearance declares no on-state cannot be
                // checked; inventing one would be a silent repair.
                outcome.refusal = DirectEditRefusal::FieldNotEditable;
                return outcome;
            }
            value = PDFObject::createName(*edit.checked ? onState : QByteArrayLiteral("Off"));
            break;
        }

        case PDFFormField::FieldType::Signature:
        case PDFFormField::FieldType::Invalid:
        default:
            outcome.refusal = DirectEditRefusal::UnsupportedFieldType;
            return outcome;
    }

    PDFDocumentModifier modifier(document);
    modifier.getBuilder()->setFormManager(&formManager);

    PDFFormField::SetValueParameters parameters;
    parameters.value = value;
    parameters.invokingWidget = field->getWidgets().empty() ? PDFObjectReference() : field->getWidgets().front().getWidget();
    parameters.invokingFormField = field;
    parameters.modifier = &modifier;
    parameters.formManager = &formManager;
    parameters.scope = PDFFormField::SetValueParameters::Scope::User;

    if (!field->setValue(parameters))
    {
        outcome.refusal = DirectEditRefusal::FieldNotEditable;
        return outcome;
    }

    // PDF semantics treat same-named terminal fields as one field, so the value
    // is carried to every other field with the same fully qualified name, as
    // the interactive edit path does. The name cannot be empty here: the entry
    // guard refused an empty request, and the match above compared against it.
    const QString qualifiedName = field->getName(PDFFormField::NameType::FullyQualified);
    parameters.scope = PDFFormField::SetValueParameters::Scope::Internal;
    formManager.modify([&parameters, &qualifiedName](PDFFormField* candidate)
                       {
                           if (parameters.invokingFormField == candidate)
                           {
                               return;
                           }

                           if (candidate->getName(PDFFormField::NameType::FullyQualified) == qualifiedName)
                           {
                               candidate->setValue(parameters);
                           } });

    if (!modifier.finalize())
    {
        outcome.refusal = DirectEditRefusal::ValueUnchanged;
        return outcome;
    }

    outcome.document = modifier.getDocument();
    return outcome;
}

bool PDFAttachmentWriter::isExecutableFileName(const QString& name)
{
    static const QStringList executableSuffixes = {
        QStringLiteral("exe"), QStringLiteral("com"), QStringLiteral("bat"), QStringLiteral("cmd"),
        QStringLiteral("msi"), QStringLiteral("msp"), QStringLiteral("scr"), QStringLiteral("pif"),
        QStringLiteral("cpl"), QStringLiteral("dll"), QStringLiteral("sys"), QStringLiteral("ps1"),
        QStringLiteral("psm1"), QStringLiteral("vbs"), QStringLiteral("vbe"), QStringLiteral("js"),
        QStringLiteral("jse"), QStringLiteral("wsf"), QStringLiteral("wsh"), QStringLiteral("hta"),
        QStringLiteral("jar"), QStringLiteral("reg"), QStringLiteral("lnk"), QStringLiteral("url"),
        QStringLiteral("app"), QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("command"), QStringLiteral("workflow"), QStringLiteral("desktop"),
        QStringLiteral("apk"), QStringLiteral("dmg"), QStringLiteral("pkg"), QStringLiteral("deb"),
        QStringLiteral("rpm"), QStringLiteral("run"), QStringLiteral("bin")
    };

    const QString suffix = QFileInfo(name).suffix().toLower();
    return !suffix.isEmpty() && executableSuffixes.contains(suffix);
}

DirectEditOutcome PDFAttachmentWriter::add(const PDFDocument* document, const AttachmentEdit& edit)
{
    DirectEditOutcome outcome;
    if (!document)
    {
        outcome.refusal = DirectEditRefusal::NoDocument;
        return outcome;
    }
    if (edit.name.isEmpty() || PDFFilenameSanitizer::sanitize(edit.name) != edit.name)
    {
        // A name this writer would have to repair is refused, not repaired.
        outcome.refusal = DirectEditRefusal::AttachmentNameInvalid;
        return outcome;
    }
    if (findEmbeddedFileKey(document, edit.name))
    {
        outcome.refusal = DirectEditRefusal::AttachmentNameTaken;
        return outcome;
    }

    PDFDocumentModifier modifier(document);
    PDFDocumentBuilder* builder = modifier.getBuilder();

    PDFDictionary streamDictionary;
    streamDictionary.addEntry(PDFInplaceOrMemoryString("Type"), PDFObject::createName("EmbeddedFile"));
    if (!edit.mimeType.isEmpty())
    {
        streamDictionary.addEntry(PDFInplaceOrMemoryString("Subtype"), PDFObject::createName(edit.mimeType));
    }
    // Streams carry their own /Length: the writer does not add one, and a reader
    // that honours the entry would otherwise reject the file this writes.
    streamDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(edit.contents.size()));
    PDFDictionary parameters;
    parameters.addEntry(PDFInplaceOrMemoryString("Size"), PDFObject::createInteger(edit.contents.size()));
    streamDictionary.addEntry(PDFInplaceOrMemoryString("Params"),
                              PDFObject::createDictionary(std::make_shared<PDFDictionary>(std::move(parameters))));
    const PDFObjectReference embeddedFile =
        builder->addObject(PDFObject::createStream(std::make_shared<PDFStream>(std::move(streamDictionary), QByteArray(edit.contents))));

    PDFDictionary fileSpecification;
    fileSpecification.addEntry(PDFInplaceOrMemoryString("Type"), PDFObject::createName("Filespec"));
    fileSpecification.addEntry(PDFInplaceOrMemoryString("F"), PDFObject::createString(edit.name.toUtf8()));
    fileSpecification.addEntry(PDFInplaceOrMemoryString("UF"), PDFObjectFactory::createTextString(edit.name));
    if (!edit.description.isEmpty())
    {
        fileSpecification.addEntry(PDFInplaceOrMemoryString("Desc"), PDFObjectFactory::createTextString(edit.description));
    }
    PDFDictionary embeddedFiles;
    embeddedFiles.addEntry(PDFInplaceOrMemoryString("F"), PDFObject::createReference(embeddedFile));
    embeddedFiles.addEntry(PDFInplaceOrMemoryString("UF"), PDFObject::createReference(embeddedFile));
    fileSpecification.addEntry(PDFInplaceOrMemoryString("EF"),
                               PDFObject::createDictionary(std::make_shared<PDFDictionary>(std::move(embeddedFiles))));
    const PDFObjectReference specification =
        builder->addObject(PDFObject::createDictionary(std::make_shared<PDFDictionary>(std::move(fileSpecification))));

    std::map<QByteArray, PDFObjectReference> entries;
    for (const auto& item : document->getCatalog()->getEmbeddedFiles())
    {
        if (!item.second.getSelfReference().isValid())
        {
            // A specification stored inline in the names array has no reference
            // to relist; rebuilding around it would silently unlist it.
            outcome.refusal = DirectEditRefusal::AttachmentSpecificationNotAddressable;
            return outcome;
        }
        entries[item.first] = item.second.getSelfReference();
    }
    entries[edit.name.toUtf8()] = specification;
    writeEmbeddedFileNamesTree(builder, entries);

    if (!modifier.finalize())
    {
        outcome.refusal = DirectEditRefusal::ValueUnchanged;
        return outcome;
    }

    outcome.document = modifier.getDocument();
    return outcome;
}

DirectEditOutcome PDFAttachmentWriter::remove(const PDFDocument* document, const QString& name)
{
    DirectEditOutcome outcome;
    if (!document)
    {
        outcome.refusal = DirectEditRefusal::NoDocument;
        return outcome;
    }
    if (name.isEmpty())
    {
        outcome.refusal = DirectEditRefusal::InvalidRequest;
        return outcome;
    }

    const std::optional<QByteArray> key = findEmbeddedFileKey(document, name);
    if (!key)
    {
        outcome.refusal = DirectEditRefusal::AttachmentNotFound;
        return outcome;
    }

    PDFDocumentModifier modifier(document);
    std::map<QByteArray, PDFObjectReference> entries;
    for (const auto& item : document->getCatalog()->getEmbeddedFiles())
    {
        if (item.first == *key)
        {
            continue;
        }
        if (!item.second.getSelfReference().isValid())
        {
            // Any other inline specification would have to be relisted, and it
            // has no reference to relist; the removal would silently drop it.
            outcome.refusal = DirectEditRefusal::AttachmentSpecificationNotAddressable;
            return outcome;
        }
        entries[item.first] = item.second.getSelfReference();
    }
    writeEmbeddedFileNamesTree(modifier.getBuilder(), entries);

    if (!modifier.finalize())
    {
        outcome.refusal = DirectEditRefusal::ValueUnchanged;
        return outcome;
    }

    outcome.document = modifier.getDocument();
    return outcome;
}

PDFAttachmentWriter::ExtractionOutcome PDFAttachmentWriter::extractToDirectory(const PDFDocument* document,
                                                                               const QString& name,
                                                                               const QString& targetDirectory)
{
    ExtractionOutcome outcome;
    if (!document)
    {
        outcome.refusal = DirectEditRefusal::NoDocument;
        outcome.typedError = QString::fromLatin1(getDirectEditRefusalCode(outcome.refusal));
        return outcome;
    }

    const QDir directory(targetDirectory);
    if (targetDirectory.isEmpty() || !directory.exists())
    {
        outcome.refusal = DirectEditRefusal::InvalidRequest;
        outcome.typedError = QStringLiteral("attachment/invalid-directory");
        return outcome;
    }

    const PDFFileSpecification* specification = findSpecification(document, name);
    const PDFEmbeddedFile* file = specification ? specification->getPlatformFile() : nullptr;
    if (!file || !file->isValid() || !file->getStream())
    {
        outcome.refusal = DirectEditRefusal::AttachmentNotFound;
        outcome.typedError = QString::fromLatin1(getDirectEditRefusalCode(outcome.refusal));
        return outcome;
    }

    const QString fileName = PDFFilenameSanitizer::sanitize(name);
    const QString path = directory.filePath(fileName);
    if (!PDFFilenameSanitizer::isPathContained(path, targetDirectory))
    {
        outcome.refusal = DirectEditRefusal::AttachmentNameInvalid;
        outcome.typedError = QString::fromLatin1(getDirectEditRefusalCode(outcome.refusal));
        return outcome;
    }

    // The atomic writer: temp file plus commit, refusing to replace an existing
    // file, so a failed extraction cannot truncate one.
    const PDFOperationResult result =
        PDFSafeFileWriter::writeData(path, *file->getStream()->getContent(), PDFSafeFileWriter::OverwritePolicy::Fail);
    if (!result)
    {
        outcome.refusal = DirectEditRefusal::WriteRefused;
        outcome.typedError = QString::fromLatin1(getDirectEditRefusalCode(outcome.refusal));
        return outcome;
    }

    outcome.written = true;
    outcome.path = path;
    outcome.autoOpenRefused = isExecutableFileName(fileName);
    return outcome;
}

QByteArray PDFMetadataWriter::readXmpPacket(const PDFDocument* document)
{
    if (!document || !document->getCatalog())
    {
        return QByteArray();
    }

    const PDFObject metadata = document->getObject(document->getCatalog()->getMetadata());
    const PDFStream* stream = metadata.isStream() ? metadata.getStream() : nullptr;
    return stream ? *stream->getContent() : QByteArray();
}

DirectEditOutcome PDFMetadataWriter::update(const PDFDocument* document, const DocumentMetadataEdit& edit)
{
    DirectEditOutcome outcome;
    if (!document)
    {
        outcome.refusal = DirectEditRefusal::NoDocument;
        return outcome;
    }
    if (!edit.title && !edit.author && !edit.subject && !edit.keywords && !edit.creator)
    {
        outcome.refusal = DirectEditRefusal::InvalidRequest;
        return outcome;
    }

    const QByteArray existingPacket = readXmpPacket(document);
    QByteArray packet;
    if (existingPacket.isEmpty())
    {
        packet = freshXmpPacket(edit);
    }
    else
    {
        const std::optional<QByteArray> merged = mergeXmpPacket(existingPacket, edit);
        if (!merged)
        {
            // Never silently replace a packet that could not be read: it may
            // carry a conformance claim or another tool's properties.
            outcome.refusal = DirectEditRefusal::XmpUnreadable;
            return outcome;
        }
        packet = *merged;
    }

    PDFDocumentModifier modifier(document);
    PDFDocumentBuilder* builder = modifier.getBuilder();

    PDFObjectFactory infoFactory;
    infoFactory.beginDictionary();
    const auto writeInfoEntry = [&infoFactory](const char* key, const std::optional<QString>& value)
    {
        if (value)
        {
            infoFactory.beginDictionaryItem(key);
            infoFactory << *value;
            infoFactory.endDictionaryItem();
        }
    };
    writeInfoEntry("Title", edit.title);
    writeInfoEntry("Author", edit.author);
    writeInfoEntry("Subject", edit.subject);
    writeInfoEntry("Keywords", edit.keywords);
    writeInfoEntry("Creator", edit.creator);
    infoFactory.endDictionary();
    builder->updateDocumentInfo(infoFactory.takeObject());

    builder->setCatalogMetadata(packet);

    if (!modifier.finalize())
    {
        outcome.refusal = DirectEditRefusal::MetadataUnchanged;
        return outcome;
    }

    outcome.document = modifier.getDocument();
    return outcome;
}

}   // namespace pdf
