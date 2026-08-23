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


#include "canvastraceoverlay.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>

namespace pdfquick
{

namespace
{

/// docs/quick-design-tokens.json, typography.small_px.
constexpr int SmallTextPx = 12;

/// docs/quick-design-tokens.json, spacing.values_px.
constexpr int PanelPaddingPx = 8;

QString formatMs(const QJsonValue& value)
{
    if (value.isNull() || value.isUndefined())
    {
        // The recorder reports an absent measurement as null on purpose. It must
        // not be rendered as "0.00", which is the one reading that would make a
        // path that never ran look like the fastest path on the panel.
        return QStringLiteral("--");
    }

    return QString::number(value.toDouble(), 'f', 2);
}

/// "p50/p95/p99" for one percentile object, or an explicit unavailable marker.
QString formatPercentiles(const QJsonObject& percentiles)
{
    if (!percentiles.value(QStringLiteral("available")).toBool(false))
    {
        return QStringLiteral("-- / -- / --  (0)");
    }

    return QStringLiteral("%1 / %2 / %3  (%4)")
        .arg(formatMs(percentiles.value(QStringLiteral("p50_ms"))),
             formatMs(percentiles.value(QStringLiteral("p95_ms"))),
             formatMs(percentiles.value(QStringLiteral("p99_ms"))),
             QString::number(percentiles.value(QStringLiteral("sample_count")).toInteger(0)));
}

QString formatBudget(const QJsonObject& budgets)
{
    const QString status = budgets.value(QStringLiteral("status")).toString();
    if (status != QStringLiteral("known"))
    {
        // The reason is a typed code from the neutral layer, not free text.
        return QStringLiteral("budget  unavailable (%1)").arg(budgets.value(QStringLiteral("reason")).toString());
    }

    return QStringLiteral("budget  %1 ms @ %2 Hz")
        .arg(QString::number(budgets.value(QStringLiteral("frame_budget_ms")).toDouble(), 'f', 2),
             QString::number(budgets.value(QStringLiteral("refresh_rate_hz")).toDouble(), 'f', 1));
}

/// The stage names, in the order TraceStage declares them, so the panel reads the
/// same way as the JSON a CI diff shows.
QString formatSlowCauses(const QJsonObject& slowCauses)
{
    QStringList parts;
    const QStringList order{ QStringLiteral("interaction"),
                             QStringLiteral("hit-test"),
                             QStringLiteral("overlay"),
                             QStringLiteral("page-surface"),
                             QStringLiteral("external"),
                             QStringLiteral("unknown") };

    for (const QString& name : order)
    {
        const QJsonValue value = slowCauses.value(name);
        if (value.isUndefined())
        {
            continue;
        }

        const qint64 count = value.toInteger(0);
        if (count > 0)
        {
            parts.append(QStringLiteral("%1 %2").arg(name, QString::number(count)));
        }
    }

    return parts.isEmpty() ? QStringLiteral("slow frames  none") : QStringLiteral("slow frames  ") + parts.join(QStringLiteral("  "));
}

}   // namespace

QStringList CanvasTraceOverlay::lines(const QJsonObject& traceSummary, const QJsonObject& presentSummary, const CanvasFrameStats& stats)
{
    QStringList result;

    const QJsonObject counts = traceSummary.value(QStringLiteral("counts")).toObject();
    const QJsonObject present = presentSummary.value(QStringLiteral("present")).toObject();

    const QString traceId = traceSummary.value(QStringLiteral("trace_id")).toString();
    result.append(traceId.isEmpty() ? QStringLiteral("canvas trace") : QStringLiteral("canvas trace  %1").arg(traceId));

    if (!traceSummary.value(QStringLiteral("enabled")).toBool(true))
    {
        result.append(QStringLiteral("recording  off"));
    }

    result.append(QStringLiteral("frames %1   inputs %2   pending %3")
                      .arg(QString::number(counts.value(QStringLiteral("frames")).toInteger(0)),
                           QString::number(counts.value(QStringLiteral("inputs")).toInteger(0)),
                           QString::number(counts.value(QStringLiteral("pending_inputs")).toInteger(0))));

    result.append(formatBudget(traceSummary.value(QStringLiteral("budgets")).toObject()));
    result.append(QStringLiteral("frame ms       ") + formatPercentiles(traceSummary.value(QStringLiteral("frame_time_ms")).toObject()));
    result.append(QStringLiteral("input>frame ms ") + formatPercentiles(traceSummary.value(QStringLiteral("input_to_frame_ms")).toObject()));
    result.append(QStringLiteral("gpu ms         ") + formatPercentiles(present.value(QStringLiteral("gpu_ms")).toObject()));
    result.append(QStringLiteral("present ms     ") + formatPercentiles(present.value(QStringLiteral("present_ms")).toObject()));
    result.append(QStringLiteral("swap gap ms    ") + formatPercentiles(present.value(QStringLiteral("frame_interval_ms")).toObject()));

    const qint64 presentedFrames = present.value(QStringLiteral("presented_frames")).toInteger(0);
    const qint64 unstamped = present.value(QStringLiteral("frames_without_render_stamp")).toInteger(0);
    if (presentedFrames > 0 || unstamped > 0)
    {
        result.append(QStringLiteral("presented %1   unstamped %2").arg(QString::number(presentedFrames), QString::number(unstamped)));
    }

    result.append(formatSlowCauses(traceSummary.value(QStringLiteral("slow_frame_causes")).toObject()));

    result.append(QStringLiteral("tiles %1 (inexact %2)").arg(QString::number(stats.tiles), QString::number(stats.inexactTiles)));
    result.append(QStringLiteral("overlay %1 (skipped %2, dropped %3, unrenderable %4)")
                      .arg(QString::number(stats.overlayPrimitives),
                           QString::number(stats.skippedPrimitives),
                           QString::number(stats.droppedPrimitives),
                           QString::number(stats.unrenderablePrimitives)));

    const QJsonObject cache = traceSummary.value(QStringLiteral("page_surface_cache")).toObject();
    result.append(QStringLiteral("surface cache  hits %1  misses %2")
                      .arg(QString::number(cache.value(QStringLiteral("hits")).toInteger(0)),
                           QString::number(cache.value(QStringLiteral("misses")).toInteger(0))));

    return result;
}

QImage CanvasTraceOverlay::render(const QJsonObject& traceSummary,
                                  const QJsonObject& presentSummary,
                                  const CanvasFrameStats& stats,
                                  const CanvasPalette& palette,
                                  qreal devicePixelRatio)
{
    const QStringList text = lines(traceSummary, presentSummary, stats);
    if (text.isEmpty())
    {
        return QImage();
    }

    const qreal ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;

    QFont font = QFont(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(SmallTextPx);

    const QFontMetrics metrics(font);
    const int lineHeight = metrics.height();

    int widest = 0;
    for (const QString& line : text)
    {
        widest = qMax(widest, metrics.horizontalAdvance(line));
    }

    const QSize logicalSize(widest + 2 * PanelPaddingPx, text.size() * lineHeight + 2 * PanelPaddingPx);

    QImage image(QSize(qRound(logicalSize.width() * ratio), qRound(logicalSize.height() * ratio)), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
    {
        return QImage();
    }

    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);

    const QRectF panel(0.0, 0.0, logicalSize.width(), logicalSize.height());
    painter.fillRect(panel, palette.hudBackground());

    // A border, because the panel sits over arbitrary page pixels and a
    // translucent fill alone does not reliably separate it from them.
    painter.setPen(palette.hudMutedText());
    painter.drawRect(panel.adjusted(0.5, 0.5, -0.5, -0.5));

    int y = PanelPaddingPx + metrics.ascent();
    for (int index = 0; index < text.size(); ++index)
    {
        painter.setPen(index == 0 ? palette.hudText() : palette.hudMutedText());
        painter.drawText(PanelPaddingPx, y, text.at(index));
        y += lineHeight;
    }

    painter.end();
    return image;
}

}   // namespace pdfquick
