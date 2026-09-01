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

#include "pdffontintegrity.h"

#include "pdfthreadaffinity.h"

#include "pdffont.h"

#include <QSet>

#include <algorithm>

namespace pdf
{

namespace
{

quint16 readU16(const QByteArray& data, int offset)
{
    return (quint16(uchar(data.at(offset))) << 8) | quint16(uchar(data.at(offset + 1)));
}

quint32 readU32(const QByteArray& data, int offset)
{
    return (quint32(uchar(data.at(offset))) << 24)
        | (quint32(uchar(data.at(offset + 1))) << 16)
        | (quint32(uchar(data.at(offset + 2))) << 8)
        | quint32(uchar(data.at(offset + 3)));
}

void inspectTrueType(const QByteArray& program, FontType fontType, QStringList& defects)
{
    if (program.size() < 12)
    {
        defects.append(QStringLiteral("TruncatedProgram"));
        return;
    }

    const QByteArray magic = program.left(4);
    if (magic != QByteArrayLiteral("OTTO")
        && magic != QByteArray::fromHex("00010000"))
    {
        defects.append(QStringLiteral("UnreadableTableDirectory"));
        return;
    }

    const quint16 tableCount = readU16(program, 4);
    const qint64 directoryEnd = 12LL + 16LL * tableCount;
    if (directoryEnd > program.size())
    {
        defects.append(QStringLiteral("UnreadableTableDirectory"));
        return;
    }

    QSet<QByteArray> tables;
    for (quint16 index = 0; index < tableCount; ++index)
    {
        const int offset = 12 + 16 * index;
        const QByteArray tag = program.mid(offset, 4);
        tables.insert(tag);
        const quint32 length = readU32(program, offset + 12);
        const quint32 tableOffset = readU32(program, offset + 8);
        if (tableOffset > quint32(program.size())
            || length > quint32(program.size()) - tableOffset)
        {
            defects.append(QStringLiteral("TruncatedProgram"));
            continue;
        }
    }

    for (const QByteArray& required : { QByteArrayLiteral("head"), QByteArrayLiteral("hhea"),
                                        QByteArrayLiteral("maxp"), QByteArrayLiteral("cmap") })
    {
        if (!tables.contains(required))
        {
            defects.append(QStringLiteral("MissingRequiredTable:%1").arg(QString::fromLatin1(required)));
        }
    }

    if (fontType == FontType::TrueType && tables.contains(QByteArrayLiteral("glyf"))
        && !tables.contains(QByteArrayLiteral("loca")))
    {
        defects.append(QStringLiteral("GlyfLocaInconsistent"));
    }
}

} // namespace

PDFFontIntegrityResult inspectPDFFontIntegrity(const PDFFont& font)
{
    // Issue #144 AC1: this is expensive and unbounded, so it must not be
    // reachable from an input handler or a frame callback. The guard does
    // not move the work -- it reports that the work is in the wrong place.
    pdf::PDFThreadAffinity::requireNotInteractive("font-scan");

    PDFFontIntegrityResult result;
    const FontDescriptor* descriptor = font.getFontDescriptor();
    result.subtype = QString::number(static_cast<int>(font.getFontType()));
    if (!descriptor || !descriptor->isEmbedded())
    {
        return result;
    }

    const QByteArray* program = descriptor->getEmbeddedFontData();
    if (!program || program->isEmpty())
    {
        result.defects.append(QStringLiteral("TruncatedProgram"));
        return result;
    }

    switch (font.getFontType())
    {
        case FontType::TrueType:
        case FontType::Type0:
            inspectTrueType(*program, font.getFontType(), result.defects);
            break;
        case FontType::Type1:
        case FontType::MMType1:
            if (!program->startsWith("%!")
                && !(program->size() >= 2 && uchar(program->at(0)) == 0x80 && uchar(program->at(1)) == 0x01))
            {
                result.defects.append(QStringLiteral("UnreadableType1Program"));
            }
            break;
        case FontType::Type3:
            result.inspectionComplete = false;
            result.defects.append(QStringLiteral("UnsupportedFormat"));
            break;
        case FontType::Invalid:
        default:
            result.inspectionComplete = false;
            result.defects.append(QStringLiteral("UnsupportedFormat"));
            break;
    }

    result.defects.removeDuplicates();
    return result;
}

} // namespace pdf
