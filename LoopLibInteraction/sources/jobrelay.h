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

#ifndef JOBRELAY_H
#define JOBRELAY_H

#include "interactionglobal.h"

#include <QObject>

#include <functional>

namespace pdfinteraction
{

/// Marshals a worker-thread job completion back onto the thread that submitted
/// it, without letting a worker touch an owner that may be gone.
///
/// A worker lambda holds a shared_ptr to the relay, so the relay always outlives
/// the job. The owner detaches the relay in its own destructor, on its own
/// thread, so a queued action that arrives afterwards finds the relay detached
/// and does nothing. Both the detach and the queued action run on the owner's
/// thread, so there is no window in which a completion can reach a destroyed
/// owner.
///
/// There is one relay type rather than one per owner. DocumentFacade and
/// PageSurfaceCoordinator have the same problem -- a scheduler worker holding a
/// callback into an object with a shorter life -- and a second implementation of
/// the detach ordering is a second chance to get it wrong.
class JobRelay final : public QObject
{
public:
    explicit JobRelay(QObject* parent = nullptr);
    ~JobRelay() override;

    /// Owner thread only.
    void detach();

    /// Callable from any thread. The action runs on the relay's thread, and only
    /// while the relay is still attached.
    void post(std::function<void()> action);

private:
    bool m_attached = true;
};

}   // namespace pdfinteraction

#endif   // JOBRELAY_H
