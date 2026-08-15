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

#include "pdfcompiler.h"
#include "pdfcms.h"
#include "pdfprogress.h"
#include "pdfexecutionpolicy.h"
#include "pdfjobscheduler.h"
#include "pdftextlayoutgenerator.h"
#include "pdfdrawspacecontroller.h"

#include <QCache>
#include <QMutexLocker>

#include "pdfdbgheap.h"

#include <algorithm>
#include <execution>
#include <map>
#include <utility>

namespace pdf
{

PDFAsynchronousPageCompiler::PDFAsynchronousPageCompiler(PDFDrawWidgetProxy* proxy) :
    BaseClass(proxy),
    m_proxy(proxy),
    m_cache(new QCache<QString, PDFPrecompiledPage>())
{
    m_cache->setMaxCost(128 * 1024 * 1024);
    connect(&PDFJobScheduler::global(), &PDFJobScheduler::jobFinished, this, [this](const PDFJobSnapshot& snapshot)
    {
        onCompileJobFinished(snapshot);
    });
}

PDFAsynchronousPageCompiler::~PDFAsynchronousPageCompiler()
{
    stop(true);

    delete m_cache;
    m_cache = nullptr;
}

bool PDFAsynchronousPageCompiler::isOperationCancelled() const
{
    return m_state == State::Stopping;
}

void PDFAsynchronousPageCompiler::start()
{
    switch (m_state)
    {
        case State::Inactive:
        {
            m_state = State::Active;
            break;
        }

        case State::Active:
            break; // We have nothing to do...

        case State::Stopping:
        {
            // We shouldn't call this function while stopping!
            Q_ASSERT(false);
            break;
        }
    }
}

void PDFAsynchronousPageCompiler::stop(bool clearCache)
{
    switch (m_state)
    {
        case State::Inactive:
        {
            Q_ASSERT(m_compileJobId.isEmpty());
            break; // We have nothing to do...
        }

        case State::Active:
        {
            m_state = State::Stopping;
            if (!m_compileJobId.isEmpty())
            {
                PDFJobScheduler::global().cancel(m_compileJobId);
                PDFJobScheduler::global().waitForFinished(m_compileJobId, 5000);
                m_compileJobId.clear();
            }
            m_tasks.clear();

            if (clearCache)
            {
                m_cache->clear();
            }

            m_state = State::Inactive;
            break;
        }

        case State::Stopping:
        {
            // We shouldn't call this function while stopping!
            Q_ASSERT(false);
            break;
        }
    }
}

void PDFAsynchronousPageCompiler::reset()
{
    stop(true);
    start();
}

void PDFAsynchronousPageCompiler::setCacheLimit(qsizetype limit)
{
    m_cache->setMaxCost(limit);
}

const PDFPrecompiledPage* PDFAsynchronousPageCompiler::getCompiledPage(PDFInteger pageIndex, bool compile)
{
    if (m_state != State::Active || !m_proxy->getDocument())
    {
        // Engine is not active, always return nullptr
        return nullptr;
    }

    const PDFRevisionIdentity revision = m_proxy->getDocumentRevision();
    const QString key = cacheKey(revision, pageIndex);
    PDFPrecompiledPage* page = m_cache->object(key);

    if (!page && compile)
    {
        bool needSubmit = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_tasks.count(pageIndex) || m_tasks.at(pageIndex).revision != revision)
            {
                m_tasks[pageIndex] = CompileTask(pageIndex, revision);
                needSubmit = true;
            }
        }
        if (needSubmit)
        {
            submitCompileJob();
        }
    }

    if (page)
    {
        page->markAccessed();
    }

    return page;
}

void PDFAsynchronousPageCompiler::smartClearCache(const int milisecondsLimit, const std::vector<PDFInteger>& activePages)
{
    if (m_state != State::Active)
    {
        // Jakub Melka: Cache clearing can be done only in active state
        return;
    }

    QMutexLocker locker(&m_mutex);

    Q_ASSERT(std::is_sorted(activePages.cbegin(), activePages.cend()));

    const PDFRevisionIdentity revision = m_proxy->getDocumentRevision();
    const QList<QString> cacheKeys = m_cache->keys();
    for (const QString& key : cacheKeys)
    {
        bool isActive = false;
        for (const PDFInteger pageIndex : activePages)
        {
            if (key == cacheKey(revision, pageIndex))
            {
                isActive = true;
                break;
            }
        }
        if (isActive)
        {
            // We do not remove active page
            continue;
        }

        const PDFPrecompiledPage* page = m_cache->object(key);
        if (page && page->hasExpired(milisecondsLimit))
        {
            m_cache->remove(key);
        }
    }
}

void PDFAsynchronousPageCompiler::submitCompileJob()
{
    if (m_state != State::Active || !m_compileJobId.isEmpty() || !m_proxy->getDocument())
    {
        return;
    }

    const PDFRevisionIdentity revision = m_proxy->getDocumentRevision();
    PDFJobSpec spec;
    spec.kind = PDFJobKind::Rendering;
    spec.priority = PDFJobPriority::VisiblePage;
    spec.documentKey = revision.document.documentId;
    spec.documentRevision = revision.toString();
    spec.operationId = QStringLiteral("page-compile");
    spec.staleResultPolicy = PDFJobStaleResultPolicy::Discard;
    PDFJobScheduler::global().setCurrentRevision(spec.documentKey, spec.documentRevision);
    m_compileJobId = PDFJobScheduler::global().submit(spec, [this](PDFJobContext& context)
    {
        std::vector<CompileTask> tasks;
        {
            QMutexLocker locker(&m_mutex);
            for (auto& task : m_tasks)
            {
                if (!task.second.finished)
                {
                    tasks.push_back(task.second);
                }
            }
        }
        if (tasks.empty() || context.isCancellationRequested())
        {
            return;
        }

        m_proxy->getFontCache()->setCacheShrinkEnabled(this, false);
        auto compilePage = [this, &context](CompileTask& task)
        {
            if (context.isCancellationRequested() || isOperationCancelled())
            {
                return;
            }
            PDFCMSPointer cms = m_proxy->getCMSManager()->getCurrentCMS();
            PDFRenderer renderer(m_proxy->getDocument(), m_proxy->getFontCache(), cms.data(),
                                 m_proxy->getOptionalContentActivity(), m_proxy->getFeatures(),
                                 m_proxy->getMeshQualitySettings());
            renderer.setOperationControl(context.operationControl());
            renderer.compile(&task.precompiledPage, task.pageIndex);
            task.finished = true;
        };
        PDFExecutionPolicy::execute(PDFExecutionPolicy::Scope::Page, tasks.begin(), tasks.end(), compilePage);
        m_proxy->getFontCache()->setCacheShrinkEnabled(this, true);

        QMutexLocker locker(&m_mutex);
        for (auto& task : tasks)
        {
            if (!task.finished)
            {
                continue;
            }
            const auto currentTask = m_tasks.find(task.pageIndex);
            if (currentTask == m_tasks.end() || currentTask->second.revision == task.revision)
            {
                m_tasks[task.pageIndex] = std::move(task);
            }
        }
    });
}

void PDFAsynchronousPageCompiler::onCompileJobFinished(const PDFJobSnapshot& snapshot)
{
    bool pending = false;
    {
        QMutexLocker locker(&m_mutex);
        if (snapshot.jobId != m_compileJobId)
        {
            return;
        }
        m_compileJobId.clear();
        for (const auto& task : m_tasks)
        {
            if (!task.second.finished)
            {
                pending = true;
                break;
            }
        }
    }
    if (snapshot.status == PDFJobStatus::Succeeded)
    {
        onPageCompiled();
    }

    bool needSubmit = false;
    {
        QMutexLocker locker(&m_mutex);
        needSubmit = pending || std::any_of(m_tasks.begin(), m_tasks.end(), [](const auto& task) { return !task.second.finished; });
    }
    if (needSubmit)
    {
        submitCompileJob();
    }
}

void PDFAsynchronousPageCompiler::onPageCompiled()
{
    std::vector<PDFInteger> compiledPages;
    std::map<PDFInteger, PDFRenderError> errors;

    {
        QMutexLocker locker(&m_mutex);

        // Search all tasks for finished tasks
        for (auto it = m_tasks.begin(); it != m_tasks.end();)
        {
            CompileTask& task = it->second;
            if (task.finished)
            {
                if (m_state == State::Active && task.revision == m_proxy->getDocumentRevision())
                {
                    // If we are in active state, try to store precompiled page
                    PDFPrecompiledPage* page = new PDFPrecompiledPage(std::move(task.precompiledPage));
                    page->markAccessed();
                    qint64 memoryConsumptionEstimate = page->getMemoryConsumptionEstimate();
                    if (m_cache->insert(cacheKey(task.revision, it->first), page, memoryConsumptionEstimate))
                    {
                        compiledPages.push_back(it->first);
                    }
                    else
                    {
                        // We can't insert page to the cache, because cache size is too small. We will
                        // emit error string to inform the user, that cache is too small.
                        QString message = PDFTranslationContext::tr("Precompiled page size is too high (%1 kB). Cache size is %2 kB. Increase the cache size!")
                                              .arg(qint64(memoryConsumptionEstimate / 1024))
                                              .arg(qint64(m_cache->maxCost() / 1024));
                        errors[it->first] = PDFRenderError(RenderErrorType::Error, message);
                    }
                }

                it = m_tasks.erase(it);
            }
            else
            {
                // Just increment the counter
                ++it;
            }
        }
    }

    for (const auto& error : errors)
    {
        Q_EMIT renderingError(error.first, { error.second });
    }

    if (!compiledPages.empty())
    {
        Q_ASSERT(std::is_sorted(compiledPages.cbegin(), compiledPages.cend()));
        Q_EMIT pageImageChanged(false, compiledPages);
    }
}

QString PDFAsynchronousPageCompiler::cacheKey(const PDFRevisionIdentity& revision, PDFInteger pageIndex) const
{
    return revision.toString() + QStringLiteral("/") + QString::number(pageIndex);
}

PDFAsynchronousTextLayoutCompiler::PDFAsynchronousTextLayoutCompiler(PDFDrawWidgetProxy* proxy) :
    BaseClass(proxy),
    m_proxy(proxy),
    m_isRunning(false),
    m_textLayoutRevision(),
    m_cache(std::bind(&PDFAsynchronousTextLayoutCompiler::createTextLayout, this, std::placeholders::_1))
{
    connect(&PDFJobScheduler::global(), &PDFJobScheduler::jobFinished, this, [this](const PDFJobSnapshot& snapshot)
    {
        onTextLayoutJobFinished(snapshot);
    });
}

void PDFAsynchronousTextLayoutCompiler::start()
{
    switch (m_state)
    {
        case State::Inactive:
        {
            m_state = State::Active;
            break;
        }

        case State::Active:
            break; // We have nothing to do...

        case State::Stopping:
        {
            // We shouldn't call this function while stopping!
            Q_ASSERT(false);
            break;
        }
    }
}

void PDFAsynchronousTextLayoutCompiler::stop(bool clearCache)
{
    switch (m_state)
    {
        case State::Inactive:
            break; // We have nothing to do...

        case State::Active:
        {
            m_state = State::Stopping;
            if (!m_textLayoutJobId.isEmpty())
            {
                PDFJobScheduler::global().cancel(m_textLayoutJobId);
                PDFJobScheduler::global().waitForFinished(m_textLayoutJobId, 5000);
                m_textLayoutJobId.clear();
            }
            m_isRunning = false;

            if (clearCache)
            {
                m_textLayouts = std::nullopt;
                m_cache.clear();
            }

            m_state = State::Inactive;
            break;
        }

        case State::Stopping:
        {
            // We shouldn't call this function while stopping!
            Q_ASSERT(false);
            break;
        }
    }
}

void PDFAsynchronousTextLayoutCompiler::reset()
{
    stop(true);
    start();
}

PDFTextLayout PDFAsynchronousTextLayoutCompiler::createTextLayout(PDFInteger pageIndex)
{
    PDFTextLayout result;

    if (isTextLayoutReady())
    {
        result = getTextLayout(pageIndex);
    }
    else
    {
        if (m_state != State::Active || !m_proxy->getDocument())
        {
            // Engine is not active, do not calculate layout
            return result;
        }

        const PDFCatalog* catalog = m_proxy->getDocument()->getCatalog();
        if (pageIndex < 0 || pageIndex >= PDFInteger(catalog->getPageCount()))
        {
            return result;
        }

        if (!catalog->getPage(pageIndex))
        {
            // Invalid page index
            return result;
        }

        const PDFPage* page = catalog->getPage(pageIndex);
        Q_ASSERT(page);

        bool guard = false;
        m_proxy->getFontCache()->setCacheShrinkEnabled(&guard, false);

        PDFCMSPointer cms = m_proxy->getCMSManager()->getCurrentCMS();
        PDFTextLayoutGenerator generator(m_proxy->getFeatures(), page, m_proxy->getDocument(), m_proxy->getFontCache(), cms.data(), m_proxy->getOptionalContentActivity(), QTransform(), m_proxy->getMeshQualitySettings());
        generator.processContents();
        result = generator.createTextLayout();
        m_proxy->getFontCache()->setCacheShrinkEnabled(&guard, true);
    }

    return result;
}

PDFTextLayout PDFAsynchronousTextLayoutCompiler::getTextLayout(PDFInteger pageIndex)
{
    if (m_state != State::Active || !m_proxy->getDocument())
    {
        // Engine is not active, always return empty layout
        return PDFTextLayout();
    }

    if (m_textLayouts)
    {
        return m_textLayouts->getTextLayout(pageIndex);
    }

    return PDFTextLayout();
}

PDFTextLayoutGetter PDFAsynchronousTextLayoutCompiler::getTextLayoutLazy(PDFInteger pageIndex)
{
    return PDFTextLayoutGetter(&m_cache, pageIndex);
}

PDFTextSelection PDFAsynchronousTextLayoutCompiler::getTextSelectionAll(QColor color) const
{
    PDFTextSelection result;

    if (m_textLayouts)
    {
        const PDFTextLayoutStorage& textLayouts = *m_textLayouts;

        QMutex mutex;
        PDFIntegerRange<size_t> pageRange(0, textLayouts.getCount());
        auto selectPageText = [&mutex, &textLayouts, &result, color](PDFInteger pageIndex)
        {
            PDFTextLayout textLayout = textLayouts.getTextLayout(pageIndex);
            PDFTextSelectionItems items;

            const PDFTextBlocks& blocks = textLayout.getTextBlocks();
            for (size_t blockId = 0, blockCount = blocks.size(); blockId < blockCount; ++blockId)
            {
                const PDFTextBlock& block = blocks[blockId];
                const PDFTextLines& lines = block.getLines();

                if (!lines.empty())
                {
                    const PDFTextLine& lastLine = lines.back();
                    Q_ASSERT(!lastLine.getCharacters().empty());

                    PDFCharacterPointer ptrStart;
                    ptrStart.pageIndex = pageIndex;
                    ptrStart.blockIndex = blockId;
                    ptrStart.lineIndex = 0;
                    ptrStart.characterIndex = 0;

                    PDFCharacterPointer ptrEnd;
                    ptrEnd.pageIndex = pageIndex;
                    ptrEnd.blockIndex = blockId;
                    ptrEnd.lineIndex = lines.size() - 1;
                    ptrEnd.characterIndex = lastLine.getCharacters().size() - 1;

                    items.emplace_back(ptrStart, ptrEnd);
                }
            }

            QMutexLocker lock(&mutex);
            result.addItems(qMove(items), color);
        };
        PDFExecutionPolicy::execute(PDFExecutionPolicy::Scope::Page, pageRange.begin(), pageRange.end(), selectPageText);
    }

    result.build();
    return result;
}

void PDFAsynchronousTextLayoutCompiler::makeTextLayout()
{
    if (m_state != State::Active || !m_proxy->getDocument())
    {
        // Engine is not active, do not calculate layout
        return;
    }

    if (m_textLayouts.has_value())
    {
        // Value is computed already
        return;
    }

    if (m_isRunning)
    {
        // Text layout is already being processed
        return;
    }

    // Jakub Melka: Mark, that we are running (test for future is not enough,
    // because future can finish before this function exits, for example)
    m_isRunning = true;
    m_textLayoutRevision = m_proxy->getDocumentRevision();

    ProgressStartupInfo info;
    info.showDialog = false;
    info.text = tr("Indexing document contents...");

    m_proxy->getFontCache()->setCacheShrinkEnabled(this, false);
    const PDFCatalog* catalog = m_proxy->getDocument()->getCatalog();
    m_proxy->getProgress()->start(catalog->getPageCount(), qMove(info));

    PDFCMSPointer cms = m_proxy->getCMSManager()->getCurrentCMS();

    auto createTextLayout = [this, cms, catalog]() -> PDFTextLayoutStorage
    {
        PDFTextLayoutStorage result(catalog->getPageCount());
        QMutex mutex;
        auto generateTextLayout = [this, &result, &mutex, cms, catalog](PDFInteger pageIndex)
        {
            if (!catalog->getPage(pageIndex))
            {
                // Invalid page index
                result.setTextLayout(pageIndex, PDFTextLayout(), &mutex);
                return;
            }

            const PDFPage* page = catalog->getPage(pageIndex);
            Q_ASSERT(page);

            PDFTextLayoutGenerator generator(m_proxy->getFeatures(), page, m_proxy->getDocument(), m_proxy->getFontCache(), cms.data(), m_proxy->getOptionalContentActivity(), QTransform(), m_proxy->getMeshQualitySettings());
            generator.processContents();
            result.setTextLayout(pageIndex, generator.createTextLayout(), &mutex);
            m_proxy->getProgress()->step();
        };

        auto pageRange = PDFIntegerRange<PDFInteger>(0, catalog->getPageCount());
        PDFExecutionPolicy::execute(PDFExecutionPolicy::Scope::Page, pageRange.begin(), pageRange.end(), generateTextLayout);
        return result;
    };

    const PDFRevisionIdentity revision = m_textLayoutRevision;
    PDFJobSpec spec;
    spec.kind = PDFJobKind::Rendering;
    spec.priority = PDFJobPriority::VisiblePage;
    spec.documentKey = revision.document.documentId;
    spec.documentRevision = revision.toString();
    spec.operationId = QStringLiteral("text-layout");
    spec.staleResultPolicy = PDFJobStaleResultPolicy::Discard;
    PDFJobScheduler::global().setCurrentRevision(spec.documentKey, spec.documentRevision);
    m_textLayoutJobId = PDFJobScheduler::global().submit(spec, [this, createTextLayout](PDFJobContext& context)
    {
        if (context.isCancellationRequested())
        {
            return;
        }
        PDFTextLayoutStorage result = createTextLayout();
        QMutexLocker locker(&m_textLayoutMutex);
        m_textLayoutJobResult = std::move(result);
    });
}

void PDFAsynchronousTextLayoutCompiler::onTextLayoutJobFinished(const PDFJobSnapshot& snapshot)
{
    if (snapshot.jobId != m_textLayoutJobId)
    {
        return;
    }
    m_textLayoutJobId.clear();
    if (snapshot.status != PDFJobStatus::Succeeded)
    {
        m_proxy->getFontCache()->setCacheShrinkEnabled(this, true);
        m_proxy->getProgress()->finish();
        m_isRunning = false;
        return;
    }
    onTextLayoutCreated();
}

void PDFAsynchronousTextLayoutCompiler::onTextLayoutCreated()
{
    m_proxy->getFontCache()->setCacheShrinkEnabled(this, true);
    m_proxy->getProgress()->finish();
    m_cache.clear();

    PDFTextLayoutStorage result;
    {
        QMutexLocker locker(&m_textLayoutMutex);
        result = std::move(m_textLayoutJobResult);
    }
    const bool isCurrent = m_proxy->getDocumentRevision() == m_textLayoutRevision;
    if (isCurrent)
    {
        m_textLayouts = std::move(result);
    }
    else
    {
        m_textLayouts = std::nullopt;
    }
    m_isRunning = false;
    if (isCurrent)
    {
        Q_EMIT textLayoutChanged();
    }
}

}   // namespace pdf
