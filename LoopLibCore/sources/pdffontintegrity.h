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

#ifndef PDFFONTINTEGRITY_H
#define PDFFONTINTEGRITY_H

#include "pdfglobal.h"

#include <QStringList>

namespace pdf
{

class PDFFont;
struct TextSequenceItem;

struct LOOPLIBCORESHARED_EXPORT PDFFontIntegrityResult
{
    QString subtype;
    QStringList defects;
    bool inspectionComplete = true;

    bool isClean() const { return inspectionComplete && defects.isEmpty(); }
};

/// Performs deterministic structural checks on an embedded font program.
/// Unsupported formats are incomplete, never clean. This helper deliberately
/// does not alter the existing embedded-fonts check contract.
LOOPLIBCORESHARED_EXPORT PDFFontIntegrityResult inspectPDFFontIntegrity(const PDFFont& font);

enum class PDFShownGlyphDefect
{
    None,
    Unresolved, ///< The shown code resolved to no glyph (TextSequence::unresolvedCodes).
    Notdef,     ///< The shown code resolved to glyph 0 (.notdef).
    EmptyOutline ///< The glyph exists but draws nothing for a visible character.
};

/// Classifies one resolved glyph item of a text sequence. Advances (TJ
/// adjustments), glyphless items and Type 3 glyph procedures are never defects
/// here; codes that resolved to nothing are reported by the text sequence.
LOOPLIBCORESHARED_EXPORT PDFShownGlyphDefect classifyShownGlyph(const TextSequenceItem& item);

LOOPLIBCORESHARED_EXPORT QString shownGlyphDefectName(PDFShownGlyphDefect defect);

} // namespace pdf

#endif // PDFFONTINTEGRITY_H
