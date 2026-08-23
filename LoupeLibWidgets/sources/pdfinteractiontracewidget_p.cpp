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

#include "pdfinteractiontracewidget_p.h"

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QStringList>
#include <QWidget>

namespace pdf
{

PDFInteractionTraceRecorder* findInteractionTraceRecorder(QObject* widget)
{
    if (!widget)
    {
        return nullptr;
    }

    return static_cast<PDFInteractionTraceRecorder*>(
        widget->findChild<QObject*>(QString::fromLatin1(PDFInteractionTraceObjectName)));
}

PDFInteractionTraceInputScope::PDFInteractionTraceInputScope(QObject* widget,
                                                             PDFInteractionTraceRecorder::InputKind kind) :
    m_recorder(findInteractionTraceRecorder(widget)),
    m_inputScope(m_recorder ? m_recorder->beginInput(kind) : PDFInteractionTraceRecorder::InputScope()),
    m_stageScope(m_recorder
                     ? m_recorder->beginStage(PDFInteractionTraceRecorder::Stage::Interaction)
                     : PDFInteractionTraceRecorder::StageScope())
{
}

namespace
{

QString traceMetric(const QJsonObject& object,
                    const QString& key,
                    const QString& unavailable = QStringLiteral("n/a"))
{
    const QJsonObject metric = object.value(key).toObject();
    if (!metric.value(QStringLiteral("available")).toBool())
    {
        return unavailable;
    }
    return QStringLiteral("%1 ms").arg(metric.value(QStringLiteral("p50_ms")).toDouble(), 0, 'f', 2);
}

}   // namespace

void drawInteractionTraceOverlay(QWidget* widget, const QJsonObject& summary)
{
    if (!widget)
    {
        return;
    }

    const QJsonObject budgets = summary.value(QStringLiteral("budgets")).toObject();
    const QJsonObject fps = summary.value(QStringLiteral("fps")).toObject();
    const QJsonObject cache = summary.value(QStringLiteral("cache")).toObject();
    const QJsonObject pending = summary.value(QStringLiteral("pending_async_work")).toObject();
    const QString budget = budgets.value(QStringLiteral("frame_budget_ms")).isNull()
                               ? QStringLiteral("unavailable")
                               : QStringLiteral("%1 ms").arg(budgets.value(QStringLiteral("frame_budget_ms")).toDouble(), 0, 'f', 2);
    const QString fpsText = fps.value(QStringLiteral("available")).toBool()
                                ? QStringLiteral("%1").arg(fps.value(QStringLiteral("p50")).toDouble(), 0, 'f', 1)
                                : QStringLiteral("n/a");
    const QString cacheText = cache.value(QStringLiteral("hit_rate")).isNull()
                                  ? QStringLiteral("n/a")
                                  : QStringLiteral("%1%%").arg(cache.value(QStringLiteral("hit_rate")).toDouble() * 100.0, 0, 'f', 0);
    const QString queueText = pending.value(QStringLiteral("queue_depth")).isNull()
                                  ? QStringLiteral("n/a")
                                  : QString::number(pending.value(QStringLiteral("queue_depth")).toInt());

    const QJsonObject stageTimes = summary.value(QStringLiteral("stage_time_ms")).toObject();
    const QStringList lines = {
        QStringLiteral("Interaction trace"),
        QStringLiteral("FPS (p50): %1    frame: %2    budget: %3")
            .arg(fpsText, traceMetric(summary, QStringLiteral("frame_time_ms")), budget),
        QStringLiteral("Input p50: %1    hit-test: %2")
            .arg(traceMetric(summary, QStringLiteral("input_to_frame_ms")), traceMetric(stageTimes, QStringLiteral("hit_testing"))),
        QStringLiteral("Page render: %1    overlays: %2")
            .arg(traceMetric(stageTimes, QStringLiteral("page_rendering")), traceMetric(stageTimes, QStringLiteral("overlays"))),
        QStringLiteral("Cache hit rate: %1    pending async: %2").arg(cacheText, queueText)
    };

    QFont font = widget->font();
    font.setPointSize(qMax(8, font.pointSize() - 1));
    QFontMetrics metrics(font);
    const int lineHeight = metrics.lineSpacing();
    const int width = qMin(widget->width() - 16, qMax(320, metrics.horizontalAdvance(lines.at(2)) + 24));
    const QRect panel(8, 8, width, lineHeight * lines.size() + 16);

    QPainter painter(widget);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.setBrush(QColor(20, 24, 32, 220));
    painter.drawRoundedRect(panel, 4.0, 4.0);
    painter.setBrush(Qt::NoBrush);
    int y = panel.top() + 8 + metrics.ascent();
    for (const QString& line : lines)
    {
        painter.drawText(panel.left() + 8, y, line);
        y += lineHeight;
    }
}

}   // namespace pdf
