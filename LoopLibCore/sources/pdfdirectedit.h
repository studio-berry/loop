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

#ifndef PDFDIRECTEDIT_H
#define PDFDIRECTEDIT_H

#include "pdfdocument.h"
#include "pdfglobal.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace pdf
{

/// Why a direct authoring edit was refused (ADR-014).
///
/// The interaction layer maps these to typed refusals; they are stable,
/// content-free codes, never document content and never a path. A refusal means
/// nothing was written: the writer builds the whole modified document in memory
/// and only hands it back when the edit was accepted in full.
enum class DirectEditRefusal
{
    None,   ///< The edit was applied; `document` carries the new revision.
    InvalidRequest,   ///< The payload is missing or ill-typed.
    NoDocument,   ///< No document was supplied.
    NoForm,   ///< The document has no interactive form.
    XfaUnsupported,   ///< XFA forms are not editable through the AcroForm writer.
    FieldNotFound,   ///< No field carries the requested fully qualified name.
    FieldNotEditable,   ///< Read-only, push button, or the writer refused the value.
    UnsupportedFieldType,   ///< Signature fields are never filled.
    ValueNotAllowed,   ///< The value is not one the field admits (e.g. a fixed choice).
    ValueUnchanged,   ///< The edit would not change the document.
    AttachmentNotFound,   ///< No embedded file carries the requested name.
    AttachmentNameTaken,   ///< An embedded file with that name already exists.
    AttachmentNameInvalid,   ///< The name is empty or would need sanitizing.
    XmpUnreadable,   ///< The existing XMP packet could not be parsed; nothing was replaced.
    MetadataUnchanged,   ///< The metadata edit would not change the document.
    WriteRefused   ///< The atomic output writer refused the target.
};

/// The stable refusal code, e.g. `form/xfa-unsupported`. `None` maps to an empty
/// string.
LOOPLIBCORESHARED_EXPORT const char* getDirectEditRefusalCode(DirectEditRefusal refusal);

/// One direct edit's outcome. Exactly one of the two shapes is true: either
/// `document` holds the new revision with `refusal == None`, or `refusal` names
/// why nothing was written.
struct LOOPLIBCORESHARED_EXPORT DirectEditOutcome
{
    PDFDocumentPointer document;
    DirectEditRefusal refusal = DirectEditRefusal::None;

    bool ok() const noexcept { return refusal == DirectEditRefusal::None && !document.isNull(); }
};

/// One form field value edit. `field` is the fully qualified field name.
/// A text or choice field takes `text`; a checkbox or radio button takes
/// `checked`. Supplying the shape the field does not admit is refused rather
/// than ignored.
struct LOOPLIBCORESHARED_EXPORT FormFieldValueEdit
{
    QString field;
    std::optional<QString> text;
    std::optional<bool> checked;
};

/// Sets one interactive form field's value. The value is written through
/// pdf::PDFDocumentModifier, so widget appearances are updated by the same
/// machinery the interactive editor uses. XFA forms are refused: their values
/// live in the XFA packet, and writing only the AcroForm mirror would be a
/// half-write.
class LOOPLIBCORESHARED_EXPORT PDFFormValueWriter
{
public:
    static DirectEditOutcome setFieldValue(const PDFDocument* document, const FormFieldValueEdit& edit);
};

/// One attachment edit. `name` is the file name shown in the EmbeddedFiles name
/// tree; the writer refuses a name that would have to be sanitized rather than
/// sanitizing it silently.
struct LOOPLIBCORESHARED_EXPORT AttachmentEdit
{
    QString name;
    QString description;
    QByteArray contents;
    QByteArray mimeType;   ///< Written as the embedded file stream's /Subtype name, e.g. `text/plain`.
};

/// Adds and removes document-level file attachments (catalog /Names /
/// EmbeddedFiles) and extracts one attachment's bytes to disk.
///
/// Add and remove rebuild the EmbeddedFiles name tree from the document's
/// parsed attachments plus the edit, and leave every other /Names entry (Dests,
/// JavaScript, ...) untouched. The file specification and embedded stream of a
/// removed attachment stay in the file as unreferenced objects; removal is the
/// document-visible act of unlisting the attachment, not object garbage
/// collection.
class LOOPLIBCORESHARED_EXPORT PDFAttachmentWriter
{
public:
    static DirectEditOutcome add(const PDFDocument* document, const AttachmentEdit& edit);
    static DirectEditOutcome remove(const PDFDocument* document, const QString& name);

    /// True when the file name carries an executable-type extension. Extracted
    /// attachments with such a name must never be handed to the operating
    /// system's opener: extraction may write the bytes the document carries,
    /// but launching them is refused.
    static bool isExecutableFileName(const QString& name);

    /// One extraction's outcome. `written` says the bytes are on disk;
    /// `autoOpenRefused` says the extracted file must not be auto-opened, which
    /// is a property of the file, not a failure of the extraction.
    struct LOOPLIBCORESHARED_EXPORT ExtractionOutcome
    {
        bool written = false;
        QString path;
        bool autoOpenRefused = false;
        DirectEditRefusal refusal = DirectEditRefusal::None;
        QString typedError;   ///< Stable code, e.g. `attachment/output-write-refused`.

        bool ok() const noexcept { return written; }
    };

    /// Writes one attachment's bytes into \p targetDirectory through
    /// pdf::PDFSafeFileWriter (atomic temp-and-rename) under a sanitized file
    /// name, refusing to overwrite an existing file. The executable check is
    /// reported, never applied by deleting or renaming the file: the bytes are
    /// the document's.
    static ExtractionOutcome extractToDirectory(const PDFDocument* document,
                                                const QString& name,
                                                const QString& targetDirectory);
};

/// One document metadata edit. Only the fields that carry a value are written;
/// an absent field is left alone, and an empty string clears the field.
struct LOOPLIBCORESHARED_EXPORT DocumentMetadataEdit
{
    std::optional<QString> title;
    std::optional<QString> author;
    std::optional<QString> subject;
    std::optional<QString> keywords;
    std::optional<QString> creator;
};

/// Updates the document information dictionary and the catalog XMP packet.
///
/// The XMP packet is merged, not replaced: every element and attribute the
/// document already carries is preserved, and only the properties this edit
/// owns (dc:title, dc:creator, dc:description, pdf:Keywords, xmp:CreatorTool)
/// are written. A packet that does not parse is refused instead of being
/// silently overwritten, because an unreadable packet may carry a conformance
/// claim. A document with no packet gets a standard one.
class LOOPLIBCORESHARED_EXPORT PDFMetadataWriter
{
public:
    static DirectEditOutcome update(const PDFDocument* document, const DocumentMetadataEdit& edit);

    /// Reads the catalog XMP packet, or an empty array when the document has
    /// none. Used by tests and by callers that must show what they are about to
    /// merge into.
    static QByteArray readXmpPacket(const PDFDocument* document);
};

}   // namespace pdf

#endif   // PDFDIRECTEDIT_H
