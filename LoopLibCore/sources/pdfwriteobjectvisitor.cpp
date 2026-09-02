// MIT License
#include "pdfwriteobjectvisitor_p.h"

#include "pdfparser.h"

namespace pdf
{

void PDFWriteObjectVisitor::visitNull()
{
    if (!checkpoint())
    {
        m_device->write("null ");
    }
}

void PDFWriteObjectVisitor::visitBool(bool value)
{
    if (!checkpoint())
    {
        m_device->write(value ? "true " : "false ");
    }
}

void PDFWriteObjectVisitor::visitInt(PDFInteger value)
{
    if (checkpoint())
    {
        return;
    }
    m_device->write(QString::number(value).toLatin1());
    m_device->write(" ");
}

void PDFWriteObjectVisitor::visitReal(PDFReal value)
{
    if (checkpoint())
    {
        return;
    }

    // PDF 1.7, appendix C, table C.1 specifies five significant digits.
    m_device->write(QString::number(value, 'f', 5).toLatin1());
    m_device->write(" ");
}

void PDFWriteObjectVisitor::visitString(PDFStringRef string)
{
    if (checkpoint())
    {
        return;
    }

    const QByteArray data = string.getString();
    if (data.contains('(') || data.contains(')') || data.contains('\\'))
    {
        m_device->write("<");
        m_device->write(data.toHex());
        m_device->write(">");
    }
    else
    {
        m_device->write("(");
        m_device->write(data);
        m_device->write(")");
    }
    m_device->write(" ");
}

void PDFWriteObjectVisitor::writeName(const QByteArray& string)
{
    if (checkpoint())
    {
        return;
    }
    m_device->write("/");

    for (const char character : string)
    {
        if (checkpoint())
        {
            return;
        }

        if (PDFLexicalAnalyzer::isRegular(character))
        {
            m_device->write(&character, 1);
        }
        else
        {
            m_device->write("#");
            m_device->write(QByteArray(&character, 1).toHex());
        }
    }
    m_device->write(" ");
}

void PDFWriteObjectVisitor::visitName(PDFStringRef name)
{
    if (!checkpoint())
    {
        writeName(name.getString());
    }
}

void PDFWriteObjectVisitor::visitArray(const PDFArray* array)
{
    if (checkpoint())
    {
        return;
    }
    m_device->write("[ ");
    acceptArray(array);
    if (!checkpoint())
    {
        m_device->write("] ");
    }
}

void PDFWriteObjectVisitor::visitDictionary(const PDFDictionary* dictionary)
{
    if (checkpoint())
    {
        return;
    }
    m_device->write("<< ");

    for (size_t i = 0, count = dictionary->getCount(); i < count; ++i)
    {
        if (checkpoint())
        {
            return;
        }
        writeName(dictionary->getKey(i).getString());
        dictionary->getValue(i).accept(this);
    }
    m_device->write(">> ");
}

void PDFWriteObjectVisitor::visitStream(const PDFStream* stream)
{
    if (checkpoint())
    {
        return;
    }
    visitDictionary(stream->getDictionary());
    if (checkpoint())
    {
        return;
    }

    m_device->write("stream\x0D\x0A");
    const QByteArray& content = *stream->getContent();
    constexpr qsizetype WriteChunkSize = 1 << 20;
    for (qsizetype offset = 0; offset < content.size(); offset += WriteChunkSize)
    {
        if (checkpoint())
        {
            return;
        }
        m_device->write(content.constData() + offset,
                        qMin(WriteChunkSize, content.size() - offset));
    }
    m_device->write("\x0D\x0A");
    m_device->write("endstream");
    m_device->write("\x0D\x0A");
}

void PDFWriteObjectVisitor::visitReference(PDFObjectReference reference)
{
    if (checkpoint())
    {
        return;
    }
    visitInt(reference.objectNumber);
    visitInt(reference.generation);
    m_device->write("R ");
}

}   // namespace pdf
