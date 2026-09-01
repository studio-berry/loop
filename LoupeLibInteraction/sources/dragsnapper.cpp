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

#include "dragsnapper.h"

#include "hittestsource.h"

#include <cmath>

namespace pdfinteraction
{

void DragSnapper::addProvider(ISnapProvider* provider)
{
    if (provider && !m_providers.contains(provider))
    {
        m_providers.push_back(provider);
    }
}

void DragSnapper::clearProviders()
{
    m_providers.clear();
}

void DragSnapper::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

void DragSnapper::setScreenThresholdPx(qreal pixels)
{
    m_screenThresholdPx = qMax(qreal(0.0), pixels);
}

qreal DragSnapper::pageThreshold(qreal viewScale) const
{
    if (!(m_screenThresholdPx > 0.0))
    {
        return 0.0;
    }

    // Same clamp as HitTestDispatcher::pageTolerance. A scale of zero is not a
    // real viewport state, and dividing by it would make every candidate on the
    // page within threshold.
    return m_screenThresholdPx / qMax(viewScale, qreal(0.01));
}

QPointF DragSnapper::snap(int pageIndex, QPointF pagePoint, qreal viewScale, QString* snappedTo) const
{
    if (snappedTo)
    {
        snappedTo->clear();
    }

    const qreal threshold = pageThreshold(viewScale);

    if (!m_enabled || pageIndex < 0 || m_providers.isEmpty() || !(threshold > 0.0))
    {
        return pagePoint;
    }

    const QRectF probe(pagePoint.x() - threshold,
                       pagePoint.y() - threshold,
                       threshold * 2.0,
                       threshold * 2.0);

    // Squared distances throughout: the comparison is the only thing that
    // matters and a square root per candidate buys nothing.
    const qreal thresholdSquared = threshold * threshold;

    QPointF best = pagePoint;
    QString bestSource;
    qreal bestDistanceSquared = thresholdSquared;
    bool found = false;

    for (const ISnapProvider* provider : m_providers)
    {
        for (const SnapCandidate& candidate : provider->snapCandidates(pageIndex, probe))
        {
            const QPointF offset = candidate.pagePoint - pagePoint;
            const qreal distanceSquared = offset.x() * offset.x() + offset.y() * offset.y();

            if (distanceSquared > bestDistanceSquared)
            {
                continue;
            }

            // Strictly-nearer wins, and an exact tie falls to the lower source
            // id. Without the tie-break the answer would depend on provider
            // registration order, which is the same determinism rule
            // HitTestDispatcher applies to candidates.
            if (found && !(distanceSquared < bestDistanceSquared) && !(candidate.sourceId < bestSource))
            {
                continue;
            }

            best = candidate.pagePoint;
            bestSource = candidate.sourceId;
            bestDistanceSquared = distanceSquared;
            found = true;
        }
    }

    if (!found)
    {
        return pagePoint;
    }

    if (snappedTo)
    {
        *snappedTo = bestSource;
    }

    return best;
}

PageBoxSnapProvider::PageBoxSnapProvider(const PageBoxHitTestSource* source) :
    m_source(source)
{
}

QList<SnapCandidate> PageBoxSnapProvider::snapCandidates(int pageIndex, const QRectF& probePageRect) const
{
    QList<SnapCandidate> candidates;

    if (!m_source || pageIndex < 0)
    {
        return candidates;
    }

    for (const InteractionTarget& box : m_source->targetsForPage(pageIndex))
    {
        const QRectF bounds = box.pageBounds;

        if (bounds.isNull())
        {
            continue;
        }

        const QPointF corners[4] = { bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight() };

        for (const QPointF& corner : corners)
        {
            // The probe is a superset filter, exactly as the spatial index is:
            // it drops corners that cannot be within threshold without
            // deciding which of the survivors is nearest.
            if (!probePageRect.contains(corner))
            {
                continue;
            }

            candidates.push_back({ corner, box.id });
        }
    }

    return candidates;
}

}   // namespace pdfinteraction
