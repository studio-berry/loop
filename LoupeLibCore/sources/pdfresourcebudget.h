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

#ifndef PDFRESOURCEBUDGET_H
#define PDFRESOURCEBUDGET_H

#include "pdfexception.h"
#include "pdfglobal.h"

#include <QJsonObject>
#include <QString>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

namespace pdf
{

enum class PDFResourcePool : std::uint8_t
{
    ActiveDocumentModel,
    CompiledEvidenceCache,
    RasterTileCache,
    GpuTextureCache,
    DecodedStreamImageCache,
    UndoHistory,
    RollbackStorage,
    Count
};

inline constexpr std::size_t PDFResourcePoolCount = static_cast<std::size_t>(PDFResourcePool::Count);

enum class PDFResourcePriority : std::uint8_t
{
    Interaction,
    Visible,
    Prefetch,
    Background
};

enum class PDFResourcePressure : std::uint8_t
{
    Normal,
    Shedding,
    Hard
};

LOUPELIBCORESHARED_EXPORT const char* getPDFResourcePoolName(PDFResourcePool pool);
LOUPELIBCORESHARED_EXPORT const char* getPDFResourcePriorityName(PDFResourcePriority priority);
LOUPELIBCORESHARED_EXPORT const char* getPDFResourcePressureName(PDFResourcePressure pressure);

struct LOUPELIBCORESHARED_EXPORT PDFResourceBudgetConfig
{
    static constexpr qsizetype MiB = 1024 * 1024;
    static constexpr qsizetype GiB = 1024 * MiB;

    /// Hard ceiling for all resident pools. Rollback storage is durable and is
    /// deliberately excluded from this resident total.
    qsizetype residentLimitBytes = 768 * MiB;

    /// Per-pool limits. Cache and history pools may shed entries before the
    /// resident ceiling is reached; active model and a visible request remain
    /// hard admission boundaries.
    std::array<qsizetype, PDFResourcePoolCount> poolLimits = {
        256 * MiB, // active document model
        128 * MiB, // compiled/evidence cache
        128 * MiB, // raster/tile cache
        128 * MiB, // GPU/texture accounted proxy
        256 * MiB, // decoded stream/image cache
        256 * MiB, // undo history
        2 * GiB    // durable rollback storage
    };

    static PDFResourceBudgetConfig conservativeDefaults();

    qsizetype limit(PDFResourcePool pool) const noexcept;
    void setLimit(PDFResourcePool pool, qsizetype bytes) noexcept;
    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFResourceUsage
{
    PDFResourcePool pool = PDFResourcePool::ActiveDocumentModel;
    qsizetype limitBytes = 0;
    qsizetype currentBytes = 0;
    qsizetype highWaterBytes = 0;
    qint64 evictions = 0;
    qint64 shed = 0;

    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFResourceBudgetExceeded
{
    PDFResourcePool pool = PDFResourcePool::ActiveDocumentModel;
    qsizetype limitBytes = 0;
    qsizetype currentBytes = 0;
    qsizetype attemptedBytes = 0;
    qsizetype residentLimitBytes = 0;
    qsizetype residentBytes = 0;
    PDFResourcePriority priority = PDFResourcePriority::Background;
    QString context;
};

class LOUPELIBCORESHARED_EXPORT PDFResourceBudgetExceededException final : public PDFException
{
public:
    explicit PDFResourceBudgetExceededException(PDFResourceBudgetExceeded detail);

    const PDFResourceBudgetExceeded& detail() const noexcept { return m_detail; }

private:
    PDFResourceBudgetExceeded m_detail;
};

class PDFResourceBudget;

/// A reservation that releases its bytes on every exit path. Reservations are
/// intentionally movable but not copyable so cache entries cannot accidentally
/// double-release a pool.
class LOUPELIBCORESHARED_EXPORT PDFResourceReservation final
{
public:
    PDFResourceReservation() = default;
    PDFResourceReservation(PDFResourceBudget* budget, PDFResourcePool pool, qsizetype bytes);
    PDFResourceReservation(std::shared_ptr<PDFResourceBudget> budget, PDFResourcePool pool, qsizetype bytes);
    ~PDFResourceReservation();

    PDFResourceReservation(const PDFResourceReservation&) = delete;
    PDFResourceReservation& operator=(const PDFResourceReservation&) = delete;

    PDFResourceReservation(PDFResourceReservation&& other) noexcept;
    PDFResourceReservation& operator=(PDFResourceReservation&& other) noexcept;

    bool isValid() const noexcept { return m_budget != nullptr && m_bytes > 0; }
    qsizetype bytes() const noexcept { return m_bytes; }
    PDFResourcePool pool() const noexcept { return m_pool; }
    void release() noexcept;

private:
    std::shared_ptr<PDFResourceBudget> m_budgetOwner;
    PDFResourceBudget* m_budget = nullptr;
    PDFResourcePool m_pool = PDFResourcePool::ActiveDocumentModel;
    qsizetype m_bytes = 0;
};

/// Thread-safe resident/durable resource accounting shared by sessions and
/// adapters. It does not depend on Qt Quick or Widgets; consumers decide how
/// to shed work after observing a failed low-priority reservation.
class LOUPELIBCORESHARED_EXPORT PDFResourceBudget final
{
public:
    explicit PDFResourceBudget(PDFResourceBudgetConfig config = PDFResourceBudgetConfig::conservativeDefaults());
    ~PDFResourceBudget() = default;

    PDFResourceBudget(const PDFResourceBudget&) = delete;
    PDFResourceBudget& operator=(const PDFResourceBudget&) = delete;

    PDFResourceBudgetConfig config() const;
    void setLimit(PDFResourcePool pool, qsizetype bytes);
    qsizetype limit(PDFResourcePool pool) const;
    qsizetype residentLimit() const;

    bool tryReserve(PDFResourcePool pool,
                    qsizetype bytes,
                    PDFResourcePriority priority = PDFResourcePriority::Background,
                    QString context = {});
    PDFResourceReservation reserve(PDFResourcePool pool,
                                    qsizetype bytes,
                                    PDFResourcePriority priority = PDFResourcePriority::Background,
                                    QString context = {});
    void release(PDFResourcePool pool, qsizetype bytes) noexcept;

    void recordEviction(PDFResourcePool pool, qsizetype bytes = 0) noexcept;
    void recordShed(PDFResourcePool pool) noexcept;

    PDFResourceUsage usage(PDFResourcePool pool) const;
    std::array<PDFResourceUsage, PDFResourcePoolCount> usages() const;
    qsizetype residentBytes() const;
    qsizetype residentHighWaterBytes() const;
    PDFResourcePressure pressure() const;
    QJsonObject toJson() const;

private:
    static std::size_t index(PDFResourcePool pool) noexcept;
    static qsizetype normalized(qsizetype bytes) noexcept;
    bool tryReserveLocked(PDFResourcePool pool,
                          qsizetype bytes,
                          PDFResourcePriority priority,
                          const QString& context,
                          PDFResourceBudgetExceeded* detail);

    PDFResourceBudgetConfig m_config;
    mutable std::mutex m_mutex;
    std::array<qsizetype, PDFResourcePoolCount> m_current{};
    std::array<qsizetype, PDFResourcePoolCount> m_highWater{};
    std::array<qint64, PDFResourcePoolCount> m_evictions{};
    std::array<qint64, PDFResourcePoolCount> m_shed{};
    qsizetype m_residentBytes = 0;
    qsizetype m_residentHighWaterBytes = 0;
};

}   // namespace pdf

#endif   // PDFRESOURCEBUDGET_H
