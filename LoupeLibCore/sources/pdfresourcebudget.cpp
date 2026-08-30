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

#include "pdfresourcebudget.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace pdf
{

namespace
{

const char* poolName(PDFResourcePool pool) noexcept
{
    switch (pool)
    {
        case PDFResourcePool::ActiveDocumentModel:
            return "active-document-model";
        case PDFResourcePool::CompiledEvidenceCache:
            return "compiled-evidence-cache";
        case PDFResourcePool::RasterTileCache:
            return "raster-tile-cache";
        case PDFResourcePool::GpuTextureCache:
            return "gpu-texture-cache";
        case PDFResourcePool::DecodedStreamImageCache:
            return "decoded-stream-image-cache";
        case PDFResourcePool::UndoHistory:
            return "undo-history";
        case PDFResourcePool::RollbackStorage:
            return "rollback-storage";
        case PDFResourcePool::Count:
            break;
    }
    return "unknown";
}

const char* priorityName(PDFResourcePriority priority) noexcept
{
    switch (priority)
    {
        case PDFResourcePriority::Interaction:
            return "interaction";
        case PDFResourcePriority::Visible:
            return "visible";
        case PDFResourcePriority::Prefetch:
            return "prefetch";
        case PDFResourcePriority::Background:
            return "background";
    }
    return "unknown";
}

const char* pressureName(PDFResourcePressure pressure) noexcept
{
    switch (pressure)
    {
        case PDFResourcePressure::Normal:
            return "normal";
        case PDFResourcePressure::Shedding:
            return "shedding";
        case PDFResourcePressure::Hard:
            return "hard";
    }
    return "unknown";
}

}   // namespace

const char* getPDFResourcePoolName(PDFResourcePool pool)
{
    return poolName(pool);
}

const char* getPDFResourcePriorityName(PDFResourcePriority priority)
{
    return priorityName(priority);
}

const char* getPDFResourcePressureName(PDFResourcePressure pressure)
{
    return pressureName(pressure);
}

PDFResourceBudgetConfig PDFResourceBudgetConfig::conservativeDefaults()
{
    return PDFResourceBudgetConfig();
}

qsizetype PDFResourceBudgetConfig::limit(PDFResourcePool pool) const noexcept
{
    const std::size_t i = static_cast<std::size_t>(pool);
    return i < poolLimits.size() ? std::max<qsizetype>(0, poolLimits[i]) : 0;
}

void PDFResourceBudgetConfig::setLimit(PDFResourcePool pool, qsizetype bytes) noexcept
{
    const std::size_t i = static_cast<std::size_t>(pool);
    if (i < poolLimits.size())
    {
        poolLimits[i] = std::max<qsizetype>(0, bytes);
    }
}

QJsonObject PDFResourceBudgetConfig::toJson() const
{
    QJsonObject pools;
    for (std::size_t i = 0; i < PDFResourcePoolCount; ++i)
    {
        const PDFResourcePool pool = static_cast<PDFResourcePool>(i);
        pools.insert(QString::fromLatin1(poolName(pool)), static_cast<qint64>(limit(pool)));
    }

    QJsonObject result;
    result.insert(QStringLiteral("resident_limit_bytes"), static_cast<qint64>(std::max<qsizetype>(0, residentLimitBytes)));
    result.insert(QStringLiteral("pool_limits_bytes"), pools);
    return result;
}

QJsonObject PDFResourceUsage::toJson() const
{
    QJsonObject result;
    result.insert(QStringLiteral("limit_bytes"), static_cast<qint64>(limitBytes));
    result.insert(QStringLiteral("current_bytes"), static_cast<qint64>(currentBytes));
    result.insert(QStringLiteral("high_water_bytes"), static_cast<qint64>(highWaterBytes));
    result.insert(QStringLiteral("evictions"), evictions);
    result.insert(QStringLiteral("shed"), shed);
    return result;
}

PDFResourceBudgetExceededException::PDFResourceBudgetExceededException(PDFResourceBudgetExceeded detail) :
    PDFException(QStringLiteral("Resource pool '%1' exceeded its %2-byte limit: current=%3, attempted=%4, resident=%5/%6, priority=%7, context=%8")
                     .arg(QString::fromLatin1(poolName(detail.pool)))
                     .arg(detail.limitBytes)
                     .arg(detail.currentBytes)
                     .arg(detail.attemptedBytes)
                     .arg(detail.residentBytes)
                     .arg(detail.residentLimitBytes)
                     .arg(QString::fromLatin1(priorityName(detail.priority)))
                     .arg(detail.context)),
    m_detail(std::move(detail))
{
}

PDFResourceReservation::PDFResourceReservation(PDFResourceBudget* budget, PDFResourcePool pool, qsizetype bytes) :
    m_budget(budget),
    m_pool(pool),
    m_bytes(bytes)
{
}

PDFResourceReservation::PDFResourceReservation(std::shared_ptr<PDFResourceBudget> budget,
                                               PDFResourcePool pool,
                                               qsizetype bytes) :
    m_budgetOwner(std::move(budget)),
    m_budget(m_budgetOwner.get()),
    m_pool(pool),
    m_bytes(bytes)
{
}

PDFResourceReservation::~PDFResourceReservation()
{
    release();
}

PDFResourceReservation::PDFResourceReservation(PDFResourceReservation&& other) noexcept :
    m_budgetOwner(std::move(other.m_budgetOwner)),
    m_budget(other.m_budget),
    m_pool(other.m_pool),
    m_bytes(other.m_bytes)
{
    other.m_budget = nullptr;
    other.m_bytes = 0;
}

PDFResourceReservation& PDFResourceReservation::operator=(PDFResourceReservation&& other) noexcept
{
    if (this != &other)
    {
        release();
        m_budgetOwner = std::move(other.m_budgetOwner);
        m_budget = other.m_budget;
        m_pool = other.m_pool;
        m_bytes = other.m_bytes;
        other.m_budget = nullptr;
        other.m_bytes = 0;
    }
    return *this;
}

void PDFResourceReservation::release() noexcept
{
    if (m_budget && m_bytes > 0)
    {
        m_budget->release(m_pool, m_bytes);
    }
    m_budget = nullptr;
    m_bytes = 0;
    m_budgetOwner.reset();
}

PDFResourceBudget::PDFResourceBudget(PDFResourceBudgetConfig config) :
    m_config(std::move(config))
{
    m_config.residentLimitBytes = std::max<qsizetype>(0, m_config.residentLimitBytes);
    for (qsizetype& limit : m_config.poolLimits)
    {
        limit = std::max<qsizetype>(0, limit);
    }
}

std::size_t PDFResourceBudget::index(PDFResourcePool pool) noexcept
{
    return static_cast<std::size_t>(pool);
}

qsizetype PDFResourceBudget::normalized(qsizetype bytes) noexcept
{
    return std::max<qsizetype>(0, bytes);
}

PDFResourceBudgetConfig PDFResourceBudget::config() const
{
    std::lock_guard lock(m_mutex);
    return m_config;
}

void PDFResourceBudget::setLimit(PDFResourcePool pool, qsizetype bytes)
{
    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    if (i >= PDFResourcePoolCount)
    {
        return;
    }

    m_config.poolLimits[i] = normalized(bytes);
}

qsizetype PDFResourceBudget::limit(PDFResourcePool pool) const
{
    std::lock_guard lock(m_mutex);
    return m_config.limit(pool);
}

qsizetype PDFResourceBudget::residentLimit() const
{
    std::lock_guard lock(m_mutex);
    return m_config.residentLimitBytes;
}

bool PDFResourceBudget::tryReserveLocked(PDFResourcePool pool,
                                         qsizetype bytes,
                                         PDFResourcePriority priority,
                                         const QString& context,
                                         PDFResourceBudgetExceeded* detail)
{
    const std::size_t i = index(pool);
    if (i >= PDFResourcePoolCount || bytes <= 0)
    {
        return i < PDFResourcePoolCount;
    }

    const qsizetype poolLimit = m_config.limit(pool);
    const bool poolOverflow = bytes > poolLimit || m_current[i] > poolLimit - bytes;
    const bool durable = pool == PDFResourcePool::RollbackStorage;
    const bool residentOverflow = !durable &&
                                  (bytes > m_config.residentLimitBytes ||
                                   m_residentBytes > m_config.residentLimitBytes - bytes);
    if (poolOverflow || residentOverflow)
    {
        if (detail)
        {
            detail->pool = pool;
            detail->limitBytes = poolLimit;
            detail->currentBytes = m_current[i];
            detail->attemptedBytes = bytes;
            detail->residentLimitBytes = m_config.residentLimitBytes;
            detail->residentBytes = m_residentBytes;
            detail->priority = priority;
            detail->context = context;
        }
        return false;
    }

    m_current[i] += bytes;
    m_highWater[i] = std::max(m_highWater[i], m_current[i]);
    if (!durable)
    {
        m_residentBytes += bytes;
        m_residentHighWaterBytes = std::max(m_residentHighWaterBytes, m_residentBytes);
    }
    return true;
}

bool PDFResourceBudget::tryReserve(PDFResourcePool pool,
                                   qsizetype bytes,
                                   PDFResourcePriority priority,
                                   QString context)
{
    std::lock_guard lock(m_mutex);
    PDFResourceBudgetExceeded detail;
    const bool result = tryReserveLocked(pool, normalized(bytes), priority, context, &detail);
    if (!result)
    {
        const std::size_t i = index(pool);
        if (i < PDFResourcePoolCount)
        {
            if (priority == PDFResourcePriority::Prefetch || priority == PDFResourcePriority::Background)
            {
                ++m_shed[i];
            }
            if (priority == PDFResourcePriority::Prefetch)
            {
                ++m_prefetchShed;
            }
        }
    }
    return result;
}

PDFResourceReservation PDFResourceBudget::reserve(PDFResourcePool pool,
                                                  qsizetype bytes,
                                                  PDFResourcePriority priority,
                                                  QString context)
{
    std::lock_guard lock(m_mutex);
    PDFResourceBudgetExceeded detail;
    const qsizetype normalizedBytes = normalized(bytes);
    if (!tryReserveLocked(pool, normalizedBytes, priority, context, &detail))
    {
        throw PDFResourceBudgetExceededException(std::move(detail));
    }
    return PDFResourceReservation(this, pool, normalizedBytes);
}

PDFResourceReservation PDFResourceBudget::reserveShared(std::shared_ptr<PDFResourceBudget> self,
                                                        PDFResourcePool pool,
                                                        qsizetype bytes,
                                                        PDFResourcePriority priority,
                                                        QString context)
{
    std::lock_guard lock(m_mutex);
    PDFResourceBudgetExceeded detail;
    const qsizetype normalizedBytes = normalized(bytes);
    if (!tryReserveLocked(pool, normalizedBytes, priority, context, &detail))
    {
        throw PDFResourceBudgetExceededException(std::move(detail));
    }
    return PDFResourceReservation(std::move(self), pool, normalizedBytes);
}

void PDFResourceBudget::release(PDFResourcePool pool, qsizetype bytes) noexcept
{
    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    if (i >= PDFResourcePoolCount || bytes <= 0)
    {
        return;
    }

    const qsizetype released = std::min(bytes, m_current[i]);
    m_current[i] -= released;
    if (pool != PDFResourcePool::RollbackStorage)
    {
        m_residentBytes -= std::min(released, m_residentBytes);
    }
}

void PDFResourceBudget::recordEviction(PDFResourcePool pool, qsizetype) noexcept
{
    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    if (i < PDFResourcePoolCount)
    {
        ++m_evictions[i];
    }
}

void PDFResourceBudget::recordShed(PDFResourcePool pool, PDFResourcePriority priority) noexcept
{
    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    if (i < PDFResourcePoolCount)
    {
        ++m_shed[i];
        if (priority == PDFResourcePriority::Prefetch)
        {
            ++m_prefetchShed;
        }
    }
}

qint64 PDFResourceBudget::prefetchShedCount() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_prefetchShed;
}

PDFResourceUsage PDFResourceBudget::usage(PDFResourcePool pool) const
{
    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    PDFResourceUsage result;
    result.pool = pool;
    if (i < PDFResourcePoolCount)
    {
        result.limitBytes = m_config.limit(pool);
        result.currentBytes = m_current[i];
        result.highWaterBytes = m_highWater[i];
        result.evictions = m_evictions[i];
        result.shed = m_shed[i];
    }
    return result;
}

std::array<PDFResourceUsage, PDFResourcePoolCount> PDFResourceBudget::usages() const
{
    std::lock_guard lock(m_mutex);
    std::array<PDFResourceUsage, PDFResourcePoolCount> result{};
    for (std::size_t i = 0; i < PDFResourcePoolCount; ++i)
    {
        const PDFResourcePool pool = static_cast<PDFResourcePool>(i);
        result[i] = { pool, m_config.limit(pool), m_current[i], m_highWater[i], m_evictions[i], m_shed[i] };
    }
    return result;
}

qsizetype PDFResourceBudget::residentBytes() const
{
    std::lock_guard lock(m_mutex);
    return m_residentBytes;
}

qsizetype PDFResourceBudget::residentHighWaterBytes() const
{
    std::lock_guard lock(m_mutex);
    return m_residentHighWaterBytes;
}

qint64 PDFResourceBudget::prefetchShedTotal() const
{
    std::lock_guard lock(m_mutex);
    return m_prefetchShed;
}

PDFResourcePressure PDFResourceBudget::pressure() const
{
    std::lock_guard lock(m_mutex);
    if (m_config.residentLimitBytes == 0 || m_residentBytes >= m_config.residentLimitBytes)
    {
        return PDFResourcePressure::Hard;
    }

    bool poolShedding = false;
    for (std::size_t i = 0; i < PDFResourcePoolCount; ++i)
    {
        if (static_cast<PDFResourcePool>(i) == PDFResourcePool::RollbackStorage)
        {
            continue;
        }
        const qsizetype poolLimit = m_config.poolLimits[i];
        if (poolLimit > 0 && m_current[i] >= (poolLimit * 85) / 100)
        {
            poolShedding = true;
            break;
        }
    }
    if (m_residentBytes >= (m_config.residentLimitBytes * 85) / 100 || poolShedding)
    {
        return PDFResourcePressure::Shedding;
    }
    return PDFResourcePressure::Normal;
}

QJsonObject PDFResourceBudget::toJson() const
{
    const auto all = usages();
    const auto configSnapshot = config();
    QJsonObject pools;
    for (const PDFResourceUsage& item : all)
    {
        pools.insert(QString::fromLatin1(poolName(item.pool)), item.toJson());
    }

    QJsonObject result;
    result.insert(QStringLiteral("config"), configSnapshot.toJson());
    result.insert(QStringLiteral("resident_bytes"), static_cast<qint64>(residentBytes()));
    result.insert(QStringLiteral("resident_high_water_bytes"), static_cast<qint64>(residentHighWaterBytes()));
    result.insert(QStringLiteral("pressure"), QString::fromLatin1(pressureName(pressure())));
    result.insert(QStringLiteral("pools"), pools);
    return result;
}

}   // namespace pdf
