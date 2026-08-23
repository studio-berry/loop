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

#include "jobrelay.h"

#include <utility>

namespace pdfinteraction
{

JobRelay::JobRelay(QObject* parent) :
    QObject(parent)
{
}

JobRelay::~JobRelay() = default;

void JobRelay::detach()
{
    m_attached = false;
}

void JobRelay::post(std::function<void()> action)
{
    if (!action)
    {
        return;
    }

    // Queued on purpose even when the caller is already on this thread: a
    // synchronous submitter must not deliver a completion while the submitting
    // call is still on the stack, or admission would run against half-built
    // state.
    QMetaObject::invokeMethod(
        this,
        [this, action = std::move(action)]()
        {
            if (m_attached)
            {
                action();
            }
        },
        Qt::QueuedConnection);
}

}   // namespace pdfinteraction
