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

#include "pdfprocessingbudget.h"

#include <limits>
#include <map>
#include <utility>

#include <QtGlobal>

namespace
{

thread_local std::map<const pdf::PDFProcessingBudget*, std::array<std::uint32_t, 2>> g_depths;

int depthIndex(pdf::PDFBudgetKind kind)
{
    switch (kind)
    {
        case pdf::PDFBudgetKind::ObjectDepth:
            return 0;

        case pdf::PDFBudgetKind::RecursiveContentDepth:
            return 1;

        default:
            return -1;
    }
}

} // namespace

namespace pdf
{

const char* getPDFBudgetKindName(PDFBudgetKind kind)
{
    switch (kind)
    {
        case PDFBudgetKind::InputBytes: return "input-bytes";
        case PDFBudgetKind::SingleDecodedStreamBytes: return "single-decoded-stream-bytes";
        case PDFBudgetKind::CumulativeDecodedBytes: return "cumulative-decoded-bytes";
        case PDFBudgetKind::DecompressionRatio: return "decompression-ratio";
        case PDFBudgetKind::ObjectDepth: return "object-depth";
        case PDFBudgetKind::RecursiveContentDepth: return "recursive-content-depth";
        case PDFBudgetKind::ObjectsVisited: return "objects-visited";
        case PDFBudgetKind::RenderOperations: return "render-operations";
        case PDFBudgetKind::RenderPixels: return "render-pixels";
        case PDFBudgetKind::ElapsedTime: return "elapsed-time";
        case PDFBudgetKind::DocumentModelBytes: return "document-model-bytes";
        case PDFBudgetKind::EvidenceCacheBytes: return "evidence-cache-bytes";
        case PDFBudgetKind::RasterTileBytes: return "raster-tile-bytes";
        case PDFBudgetKind::UndoBytes: return "undo-bytes";
        case PDFBudgetKind::RollbackBytes: return "rollback-bytes";
    }

    return "unknown";
}

PDFProcessingLimits PDFProcessingLimits::conservativeDefaults()
{
    return PDFProcessingLimits();
}

PDFBudgetExceededException::PDFBudgetExceededException(PDFBudgetExceeded detail) :
    PDFException(QStringLiteral("PDF processing budget '%1' exceeded: attempted %2, limit %3 (%4).")
                     .arg(QString::fromLatin1(getPDFBudgetKindName(detail.kind)))
                     .arg(detail.attempted)
                     .arg(detail.limit)
                     .arg(detail.context)),
    m_detail(std::move(detail))
{
}

PDFProcessingBudget::PDFProcessingBudget(PDFProcessingLimits limits) :
    PDFProcessingBudget(std::move(limits), [] { return std::chrono::steady_clock::now(); })
{
}

PDFProcessingBudget::PDFProcessingBudget(PDFProcessingLimits limits, ClockNow now) :
    m_limits(std::move(limits)),
    m_now(std::move(now)),
    m_started()
{
    if (!m_now)
    {
        m_now = [] { return std::chrono::steady_clock::now(); };
    }

    reset();
}

void PDFProcessingBudget::reset()
{
    m_inputBytes = 0;
    m_cumulativeDecodedBytes = 0;
    m_objectsVisited = 0;
    m_renderOperations = 0;
    m_renderPixels = 0;
    m_documentModelBytes = 0;
    m_evidenceCacheBytes = 0;
    m_rasterTileBytes = 0;
    m_undoBytes = 0;
    m_rollbackBytes = 0;
    m_started = m_now();
}

void PDFProcessingBudget::setLimits(PDFProcessingLimits limits)
{
    m_limits = std::move(limits);
    reset();
}

std::uint64_t PDFProcessingBudget::asLimit(std::int64_t limit)
{
    return limit < 0 ? 0 : static_cast<std::uint64_t>(limit);
}

void PDFProcessingBudget::chargeCounter(std::atomic<std::uint64_t>& counter,
                                        std::uint64_t delta,
                                        PDFBudgetKind kind,
                                        std::uint64_t limit,
                                        const QString& context)
{
    std::uint64_t current = counter.load(std::memory_order_relaxed);
    while (true)
    {
        if (delta > limit || current > limit - delta)
        {
            fail(kind, limit, current > std::numeric_limits<std::uint64_t>::max() - delta
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : current + delta,
                 context);
        }

        if (counter.compare_exchange_weak(current,
                                          current + delta,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed))
        {
            return;
        }
    }
}

void PDFProcessingBudget::chargeInputBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_inputBytes, bytes, PDFBudgetKind::InputBytes, asLimit(m_limits.maxInputBytes), context);
}

void PDFProcessingBudget::checkDecodedStreamSize(std::uint64_t decodedBytes,
                                                 std::uint64_t compressedBytes,
                                                 QString context) const
{
    const std::uint64_t singleLimit = asLimit(m_limits.maxDecodedStreamBytes);
    if (decodedBytes > singleLimit)
    {
        fail(PDFBudgetKind::SingleDecodedStreamBytes, singleLimit, decodedBytes, context);
    }

    const std::uint64_t ratio = asLimit(m_limits.maxDecompressionRatio);
    if (ratio == 0)
    {
        if (decodedBytes != 0)
        {
            fail(PDFBudgetKind::DecompressionRatio, 0, decodedBytes, context);
        }
    }
    else if (compressedBytes != 0
             && (compressedBytes > std::numeric_limits<std::uint64_t>::max() / ratio
                 || decodedBytes > compressedBytes * ratio))
    {
        fail(PDFBudgetKind::DecompressionRatio, ratio, decodedBytes, context);
    }
}

void PDFProcessingBudget::chargeDecodedBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_cumulativeDecodedBytes,
                   bytes,
                   PDFBudgetKind::CumulativeDecodedBytes,
                   asLimit(m_limits.maxCumulativeDecodedBytes),
                   context);
}

void PDFProcessingBudget::chargeObject(QString context)
{
    chargeCounter(m_objectsVisited,
                   1,
                   PDFBudgetKind::ObjectsVisited,
                   m_limits.maxObjectsVisited,
                   context);
}

void PDFProcessingBudget::chargeRenderOperation(std::uint64_t count, QString context)
{
    chargeCounter(m_renderOperations,
                   count,
                   PDFBudgetKind::RenderOperations,
                   m_limits.maxRenderOperations,
                   context);
}

void PDFProcessingBudget::chargeRenderPixels(std::uint64_t pixels, QString context)
{
    chargeCounter(m_renderPixels,
                   pixels,
                   PDFBudgetKind::RenderPixels,
                   m_limits.maxRenderPixels,
                   context);
}

void PDFProcessingBudget::chargeDocumentModelBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_documentModelBytes, bytes, PDFBudgetKind::DocumentModelBytes, asLimit(m_limits.maxDocumentModelBytes), context);
}

void PDFProcessingBudget::chargeEvidenceCacheBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_evidenceCacheBytes, bytes, PDFBudgetKind::EvidenceCacheBytes, asLimit(m_limits.maxEvidenceCacheBytes), context);
}

void PDFProcessingBudget::chargeRasterTileBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_rasterTileBytes, bytes, PDFBudgetKind::RasterTileBytes, asLimit(m_limits.maxRasterTileBytes), context);
}

void PDFProcessingBudget::chargeUndoBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_undoBytes, bytes, PDFBudgetKind::UndoBytes, asLimit(m_limits.maxUndoBytes), context);
}

void PDFProcessingBudget::chargeRollbackBytes(std::uint64_t bytes, QString context)
{
    chargeCounter(m_rollbackBytes, bytes, PDFBudgetKind::RollbackBytes, asLimit(m_limits.maxRollbackBytes), context);
}

void PDFProcessingBudget::checkElapsed(QString context) const
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(m_now() - m_started);
    const auto limit = m_limits.maxElapsed.count() < 0 ? 0ULL : static_cast<std::uint64_t>(m_limits.maxElapsed.count());
    const auto attempted = elapsed.count() < 0 ? 0ULL : static_cast<std::uint64_t>(elapsed.count());
    if (attempted > limit)
    {
        fail(PDFBudgetKind::ElapsedTime, limit, attempted, context);
    }
}

std::uint32_t& PDFProcessingBudget::threadDepth(const PDFProcessingBudget* budget, PDFBudgetKind kind)
{
    const int index = depthIndex(kind);
    Q_ASSERT(index >= 0);
    return g_depths[budget][static_cast<size_t>(index)];
}

PDFProcessingBudget::DepthScope::DepthScope(PDFProcessingBudget& budget,
                                            PDFBudgetKind kind,
                                            QString context) :
    m_budget(&budget),
    m_kind(kind)
{
    const std::uint64_t limit = kind == PDFBudgetKind::ObjectDepth
                                    ? budget.limits().maxObjectDepth
                                    : budget.limits().maxRecursiveContentDepth;
    m_depth = &PDFProcessingBudget::threadDepth(&budget, kind);
    if (*m_depth >= limit)
    {
        budget.fail(kind, limit, static_cast<std::uint64_t>(*m_depth) + 1, context);
    }

    ++*m_depth;
}

PDFProcessingBudget::DepthScope::~DepthScope()
{
    if (m_budget && m_depth)
    {
        Q_ASSERT(*m_depth > 0);
        --*m_depth;
    }
}

[[noreturn]] void PDFProcessingBudget::fail(PDFBudgetKind kind,
                                            std::uint64_t limit,
                                            std::uint64_t attempted,
                                            const QString& context) const
{
    throw PDFBudgetExceededException({ kind, limit, attempted, context });
}

} // namespace pdf
