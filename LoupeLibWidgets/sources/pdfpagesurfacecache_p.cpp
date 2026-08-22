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

#include "pdfpagesurfacecache_p.h"

#include <limits>
#include <iterator>
#include <tuple>
#include <utility>

namespace pdf
{

bool PDFPageSurfaceKey::operator<(const PDFPageSurfaceKey& other) const
{
    return std::tuple(revision, pageIndex, rotation, featureBits,
                      targetPixelSize.width(), targetPixelSize.height(), devicePixelRatio1000)
        < std::tuple(other.revision, other.pageIndex, other.rotation, other.featureBits,
                     other.targetPixelSize.width(), other.targetPixelSize.height(), other.devicePixelRatio1000);
}

bool PDFPageSurfaceKey::compatibleWith(const PDFPageSurfaceKey& desired) const
{
    return revision == desired.revision && pageIndex == desired.pageIndex && rotation == desired.rotation
        && featureBits == desired.featureBits && devicePixelRatio1000 == desired.devicePixelRatio1000;
}

PDFPageSurfaceCache::PDFPageSurfaceCache(qsizetype byteBudget) :
    m_byteBudget(qMax<qsizetype>(0, byteBudget))
{
}

void PDFPageSurfaceCache::setByteBudget(qsizetype byteBudget)
{
    m_byteBudget = qMax<qsizetype>(0, byteBudget);
    trimToBudget();
}

void PDFPageSurfaceCache::clear()
{
    m_entries.clear();
    m_currentBytes = 0;
    ++m_generation;
}

quint64 PDFPageSurfaceCache::beginRequest()
{
    return ++m_generation;
}

std::optional<PDFPageSurfaceLookup> PDFPageSurfaceCache::lookup(const PDFPageSurfaceKey& desired)
{
    Entry* best = nullptr;
    bool exact = false;
    qint64 bestDistance = std::numeric_limits<qint64>::max();

    for (auto& item : m_entries)
    {
        Entry& entry = item.second;
        if (!entry.key.compatibleWith(desired))
        {
            continue;
        }

        const qint64 widthDistance = qAbs(entry.key.targetPixelSize.width() - desired.targetPixelSize.width());
        const qint64 heightDistance = qAbs(entry.key.targetPixelSize.height() - desired.targetPixelSize.height());
        const qint64 distance = widthDistance + heightDistance;
        const bool itemExact = entry.key.targetPixelSize == desired.targetPixelSize;
        if (!best || (itemExact && !exact) || (itemExact == exact && distance < bestDistance)
            || (itemExact == exact && distance == bestDistance && entry.accessSequence < best->accessSequence))
        {
            best = &entry;
            exact = itemExact;
            bestDistance = distance;
        }
    }

    if (!best)
    {
        return std::nullopt;
    }

    best->accessSequence = ++m_accessSequence;
    return PDFPageSurfaceLookup { best->image, exact };
}

bool PDFPageSurfaceCache::insert(const PDFPageSurfaceKey& key, quint64 generation, QImage image)
{
    if (generation != m_generation || image.isNull())
    {
        return false;
    }

    const qsizetype cost = image.sizeInBytes();
    if (cost <= 0 || cost > m_byteBudget)
    {
        return false;
    }

    if (auto existing = m_entries.find(key); existing != m_entries.end())
    {
        m_currentBytes -= existing->second.cost;
        m_entries.erase(existing);
    }

    Entry entry;
    entry.key = key;
    entry.image = std::move(image);
    entry.cost = cost;
    entry.accessSequence = ++m_accessSequence;
    m_entries.emplace(key, std::move(entry));
    m_currentBytes += cost;
    trimToBudget();
    return m_entries.find(key) != m_entries.end();
}

void PDFPageSurfaceCache::trimToBudget()
{
    while (m_currentBytes > m_byteBudget && !m_entries.empty())
    {
        auto oldest = m_entries.begin();
        for (auto it = std::next(m_entries.begin()); it != m_entries.end(); ++it)
        {
            if (it->second.accessSequence < oldest->second.accessSequence)
            {
                oldest = it;
            }
        }

        m_currentBytes -= oldest->second.cost;
        m_entries.erase(oldest);
    }
}

}   // namespace pdf
