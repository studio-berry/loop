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

#ifndef PDFPAGECACHEBUDGET_H
#define PDFPAGECACHEBUDGET_H

#include "pdfglobal.h"

#include <QtGlobal>

#include <array>
#include <mutex>

namespace pdf
{

/// Shared resident-byte authority for compiled pages and admitted page
/// surfaces.  The partition is deliberately owned here rather than repeated
/// by the two cache consumers: every successful reservation contributes to the
/// same resident total and therefore cannot make the combined cache exceed the
/// configured limit.
class LOOPLIBCORESHARED_EXPORT PDFPageCacheBudget final
{
public:
    enum class Pool : unsigned char
    {
        CompiledPages,
        PageSurfaces,
        Count
    };

    static constexpr qsizetype DefaultTotal = 128ll * 1024 * 1024;

    explicit PDFPageCacheBudget(qsizetype requested = DefaultTotal) noexcept;

    PDFPageCacheBudget(const PDFPageCacheBudget&) = delete;
    PDFPageCacheBudget& operator=(const PDFPageCacheBudget&) = delete;

    static qsizetype total(qsizetype requested) { return qMax<qsizetype>(0, requested); }
    static qsizetype compiledPages(qsizetype requested) { return total(requested) / 2; }
    static qsizetype pageSurfaces(qsizetype requested)
    {
        const qsizetype t = total(requested);
        return t - compiledPages(t);
    }

    qsizetype total() const noexcept;
    qsizetype compiledLimit() const noexcept;
    qsizetype pageSurfacesLimit() const noexcept;

    /// Changes the configured ceiling.  Consumers trim their own caches after
    /// this call; existing reservations remain visible until they are released.
    void setTotal(qsizetype requested) noexcept;

    /// Reserves resident bytes in one partition.  The pool limit and the
    /// combined resident ceiling are checked atomically.
    bool tryReserve(Pool pool, qsizetype bytes) noexcept;
    void release(Pool pool, qsizetype bytes) noexcept;
    qsizetype usage(Pool pool) const noexcept;
    qsizetype residentBytes() const noexcept;

private:
    static constexpr std::size_t index(Pool pool) noexcept
    {
        return static_cast<std::size_t>(pool);
    }

    mutable std::mutex m_mutex;
    qsizetype m_total = 0;
    std::array<qsizetype, static_cast<std::size_t>(Pool::Count)> m_current{};
    qsizetype m_resident = 0;
};

}   // namespace pdf

#endif   // PDFPAGECACHEBUDGET_H
