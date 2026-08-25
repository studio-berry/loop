// MIT License
#include "pdftoolstructuredoutput.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace pdftool
{

namespace
{

QString scalarText(const QJsonValue& value)
{
    if (value.isNull() || value.isUndefined())
    {
        return QStringLiteral("null");
    }
    if (value.isBool())
    {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isString())
    {
        return value.toString();
    }
    return value.toVariant().toString();
}

void appendValue(PDFOutputFormatter& formatter, const QString& name, const QJsonValue& value)
{
    if (value.isObject())
    {
        formatter.beginHeader(name, name);
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            appendValue(formatter, it.key(), it.value());
        }
        formatter.endHeader();
        return;
    }

    if (value.isArray())
    {
        formatter.beginHeader(name, name);
        for (const QJsonValue& item : value.toArray())
        {
            appendValue(formatter, QStringLiteral("item"), item);
        }
        formatter.endHeader();
        return;
    }

    formatter.writeText(name, scalarText(value));
}

}   // namespace

QString formatStructuredObject(const QJsonObject& object,
                               PDFOutputFormatter::Style style,
                               const QString& rootName)
{
    if (style == PDFOutputFormatter::Style::Json)
    {
        return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
    }

    PDFOutputFormatter formatter(style);
    formatter.beginDocument(rootName, rootName);
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        appendValue(formatter, it.key(), it.value());
    }
    formatter.endDocument();
    return formatter.getString();
}

}   // namespace pdftool
