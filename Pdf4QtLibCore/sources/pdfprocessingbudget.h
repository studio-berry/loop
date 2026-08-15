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

#ifndef PDFPROCESSINGBUDGET_H
#define PDFPROCESSINGBUDGET_H

#include "pdfexception.h"
#include "pdfglobal.h"

#include <QString>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace pdf
{

enum class PDFBudgetKind
{
    InputBytes,
    SingleDecodedStreamBytes,
    CumulativeDecodedBytes,
    DecompressionRatio,
    ObjectDepth,
    RecursiveContentDepth,
    ObjectsVisited,
    RenderOperations,
    RenderPixels,
    ElapsedTime,
    EvidenceRecords,
    UndoSnapshots,
    RollbackArtifacts
};

/// Named finite pools. Individual kinds remain the exact exhaustion reason.
enum class PDFBudgetPool
{
    DocumentModel,
    EvidenceCache,
    RasterTile,
    DecodedStreams,
    Undo,
    Rollback
};

PDF4QTLIBCORESHARED_EXPORT const char* getPDFBudgetKindName(PDFBudgetKind kind);
PDF4QTLIBCORESHARED_EXPORT const char* getPDFBudgetPoolName(PDFBudgetPool pool);
PDF4QTLIBCORESHARED_EXPORT PDFBudgetPool budgetPoolFor(PDFBudgetKind kind);

struct PDF4QTLIBCORESHARED_EXPORT PDFProcessingLimits
{
    std::int64_t maxInputBytes = 1LL * 1024 * 1024 * 1024;
    std::int64_t maxDecodedStreamBytes = 256LL * 1024 * 1024;
    std::int64_t maxCumulativeDecodedBytes = 1LL * 1024 * 1024 * 1024;
    std::int64_t maxDecompressionRatio = 256;

    std::uint32_t maxObjectDepth = 128;
    std::uint32_t maxRecursiveContentDepth = 64;
    std::uint64_t maxObjectsVisited = 5'000'000;

    std::uint64_t maxRenderOperations = 20'000'000;
    std::uint64_t maxRenderPixels = 500'000'000;
    std::chrono::milliseconds maxElapsed = std::chrono::minutes(2);

    std::uint64_t maxEvidenceRecords = 2'000'000;
    std::uint64_t maxUndoSnapshots = 64;
    std::uint64_t maxRollbackArtifacts = 256;

    static PDFProcessingLimits conservativeDefaults();
};

struct PDF4QTLIBCORESHARED_EXPORT PDFBudgetExceeded
{
    PDFBudgetKind kind = PDFBudgetKind::ElapsedTime;
    PDFBudgetPool pool = PDFBudgetPool::DocumentModel;
    std::uint64_t limit = 0;
    std::uint64_t attempted = 0;
    QString context;
};

class PDF4QTLIBCORESHARED_EXPORT PDFBudgetExceededException final : public PDFException
{
public:
    explicit PDFBudgetExceededException(PDFBudgetExceeded detail);

    const PDFBudgetExceeded& getDetail() const noexcept { return m_detail; }

private:
    PDFBudgetExceeded m_detail;
};

/// Operation-local accounting for hostile PDF processing.
///
/// The limits are immutable for the lifetime of a budget. Counters are atomic
/// so a caller can safely share one operation budget across page workers. Depth
/// scopes remain thread-local because parallel pages have independent call
/// stacks while contributing to the same document-wide work counters.
class PDF4QTLIBCORESHARED_EXPORT PDFProcessingBudget
{
public:
    using ClockNow = std::function<std::chrono::steady_clock::time_point()>;

    explicit PDFProcessingBudget(PDFProcessingLimits limits = PDFProcessingLimits::conservativeDefaults());
    PDFProcessingBudget(PDFProcessingLimits limits, ClockNow now);

    PDFProcessingBudget(const PDFProcessingBudget&) = delete;
    PDFProcessingBudget& operator=(const PDFProcessingBudget&) = delete;

    void reset();
    void setLimits(PDFProcessingLimits limits);

    const PDFProcessingLimits& limits() const noexcept { return m_limits; }

    void chargeInputBytes(std::uint64_t bytes, QString context = {});
    void checkDecodedStreamSize(std::uint64_t decodedBytes,
                                std::uint64_t compressedBytes,
                                QString context = {}) const;
    void chargeDecodedBytes(std::uint64_t bytes, QString context = {});
    void chargeObject(QString context = {});
    void chargeRenderOperation(std::uint64_t count = 1, QString context = {});
    void chargeRenderPixels(std::uint64_t pixels, QString context = {});
    void checkElapsed(QString context = {}) const;
    void chargeEvidenceRecords(std::uint64_t count = 1, QString context = {});
    void chargeUndoSnapshot(QString context = {});
    void chargeRollbackArtifact(QString context = {});

    class PDF4QTLIBCORESHARED_EXPORT DepthScope
    {
    public:
        DepthScope(PDFProcessingBudget& budget, PDFBudgetKind kind, QString context = {});
        ~DepthScope();

        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;

    private:
        PDFProcessingBudget* m_budget = nullptr;
        PDFBudgetKind m_kind = PDFBudgetKind::ObjectDepth;
        std::uint32_t* m_depth = nullptr;
    };

private:
    friend class DepthScope;

    static std::uint64_t asLimit(std::int64_t limit);
    static std::uint32_t& threadDepth(const PDFProcessingBudget* budget, PDFBudgetKind kind);

    void chargeCounter(std::atomic<std::uint64_t>& counter,
                       std::uint64_t delta,
                       PDFBudgetKind kind,
                       std::uint64_t limit,
                       const QString& context);

    [[noreturn]] void fail(PDFBudgetKind kind,
                           std::uint64_t limit,
                           std::uint64_t attempted,
                           const QString& context) const;

    PDFProcessingLimits m_limits;
    ClockNow m_now;
    std::chrono::steady_clock::time_point m_started;
    std::atomic<std::uint64_t> m_inputBytes = 0;
    std::atomic<std::uint64_t> m_cumulativeDecodedBytes = 0;
    std::atomic<std::uint64_t> m_objectsVisited = 0;
    std::atomic<std::uint64_t> m_renderOperations = 0;
    std::atomic<std::uint64_t> m_renderPixels = 0;
    std::atomic<std::uint64_t> m_evidenceRecords = 0;
    std::atomic<std::uint64_t> m_undoSnapshots = 0;
    std::atomic<std::uint64_t> m_rollbackArtifacts = 0;
};

} // namespace pdf

#endif // PDFPROCESSINGBUDGET_H
