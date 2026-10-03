// MIT License
#ifndef DOCUMENTOPERATORPRESENTATION_H
#define DOCUMENTOPERATORPRESENTATION_H

#include "documentfacade.h"
#include "pdfartifactidentity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QVariantMap>

namespace pdfinteraction
{

inline QJsonObject inspectionPresentation(const DocumentOperatorState& state)
{
    QStringList intents;
    QStringList intentLabels;
    const auto label = [](const char* text)
    { return QCoreApplication::translate("InspectionPresentation", text); };
    if (state.canActOnInspection())
    {
        intents.append(QStringLiteral("selectFinding"));
        intentLabels.append(label("Select finding"));
        if (!state.inspection.selectedFindingId.isEmpty())
        {
            intents.append(QStringLiteral("requestPlan"));
            intentLabels.append(label("Request plan"));
        }
    }
    if (state.inspection.planIntent)
    {
        intents.append(QStringLiteral("clearPlanIntent"));
        intentLabels.append(label("Clear plan intent"));
    }

    QJsonObject presentation{
        { QStringLiteral("hasReceipt"), state.inspection.receipt.has_value() },
        { QStringLiteral("findingIds"), QJsonArray::fromStringList(state.inspection.findingIds) },
        { QStringLiteral("availableIntents"), QJsonArray::fromStringList(intents) }
    };
    if (!state.inspection.receipt)
    {
        presentation.insert(QStringLiteral("lines"), QJsonArray{});
        return presentation;
    }

    const pdf::PreflightInspectionReceipt& receipt = *state.inspection.receipt;
    const QJsonObject receiptJson = receipt.toJson();
    presentation.insert(QStringLiteral("receipt"), receiptJson);
    presentation.insert(QStringLiteral("receiptSha256"), QString::fromLatin1(
                                                             QCryptographicHash::hash(pdf::canonicalJson(receiptJson), QCryptographicHash::Sha256).toHex()));
    presentation.insert(QStringLiteral("verdict"), receipt.verdict.toJson());
    presentation.insert(QStringLiteral("coverage"), receipt.coverageScope);
    presentation.insert(QStringLiteral("limitations"), QJsonArray::fromStringList(receipt.limitations));
    const QStringList lines{
        label("Verdict: %1").arg(pdf::preflightVerdictOperatorSummary(receipt.verdict)),
        label("Coverage: %1").arg(QString::fromUtf8(pdf::canonicalJson(receipt.coverageScope))),
        label("Limitations: %1").arg(receipt.limitations.join(QStringLiteral("; "))),
        label("Findings: %1").arg(state.inspection.findingIds.join(QStringLiteral(", "))),
        label("Available intents: %1").arg(intentLabels.join(QStringLiteral(", ")))
    };
    presentation.insert(QStringLiteral("lines"), QJsonArray::fromStringList(lines));
    return presentation;
}

inline QVariantMap editorInspectionPresentation(const DocumentOperatorState& state)
{
    return inspectionPresentation(state).toVariantMap();
}

inline QByteArray headlessInspectionPresentation(const DocumentOperatorState& state)
{
    const QJsonObject presentation = inspectionPresentation(state);
    QStringList lines;
    for (const QJsonValue& line : presentation.value(QStringLiteral("lines")).toArray())
    {
        lines.append(line.toString());
    }
    return lines.join(QLatin1Char('\n')).toUtf8();
}

}   // namespace pdfinteraction

#endif   // DOCUMENTOPERATORPRESENTATION_H
