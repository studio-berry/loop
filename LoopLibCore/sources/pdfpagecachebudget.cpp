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

#include "pdfpagecachebudget.h"

namespace pdf
{

PDFPageCacheBudget::PDFPageCacheBudget(qsizetype requested) noexcept :
    m_total(total(requested))
{
}

qsizetype PDFPageCacheBudget::total() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_total;
}

qsizetype PDFPageCacheBudget::compiledLimit() const noexcept
{
    std::lock_guard lock(m_mutex);
    return compiledPages(m_total);
}

qsizetype PDFPageCacheBudget::pageSurfacesLimit() const noexcept
{
    std::lock_guard lock(m_mutex);
    return pageSurfaces(m_total);
}

void PDFPageCacheBudget::setTotal(qsizetype requested) noexcept
{
    std::lock_guard lock(m_mutex);
    m_total = total(requested);
}

bool PDFPageCacheBudget::tryReserve(Pool pool, qsizetype bytes) noexcept
{
    if (bytes <= 0)
    {
        return true;
    }

    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    if (i >= m_current.size())
    {
        return false;
    }

    const qsizetype limit = i == index(Pool::CompiledPages) ? compiledPages(m_total) : pageSurfaces(m_total);
    if (bytes > limit || m_current[i] > limit - bytes || bytes > m_total || m_resident > m_total - bytes)
    {
        return false;
    }

    m_current[i] += bytes;
    m_resident += bytes;
    return true;
}

void PDFPageCacheBudget::release(Pool pool, qsizetype bytes) noexcept
{
    if (bytes <= 0)
    {
        return;
    }

    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    if (i >= m_current.size())
    {
        return;
    }

    const qsizetype released = qMin(bytes, m_current[i]);
    m_current[i] -= released;
    m_resident -= qMin(released, m_resident);
}

qsizetype PDFPageCacheBudget::usage(Pool pool) const noexcept
{
    std::lock_guard lock(m_mutex);
    const std::size_t i = index(pool);
    return i < m_current.size() ? m_current[i] : 0;
}

qsizetype PDFPageCacheBudget::residentBytes() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_resident;
}

}   // namespace pdf
