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

#ifndef PDFTOOLCANCEL_H
#define PDFTOOLCANCEL_H

#include <atomic>
#include <chrono>

#include <QtGlobal>

namespace pdftool
{

inline std::atomic_bool& cancelRequested()
{
    static std::atomic_bool flag{ false };
    return flag;
}

inline std::atomic<qint64>& cancellationRequestedAtMs()
{
    static std::atomic<qint64> timestamp{ -1 };
    return timestamp;
}

inline qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline bool isCancelRequested()
{
    return cancelRequested().load(std::memory_order_acquire);
}

inline void requestCancellation()
{
    qint64 unset = -1;
    cancellationRequestedAtMs().compare_exchange_strong(unset,
                                                        monotonicMilliseconds(),
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire);
    cancelRequested().store(true, std::memory_order_release);
}

inline qint64 cancellationLatencyMs()
{
    const qint64 requestedAt = cancellationRequestedAtMs().load(std::memory_order_acquire);
    return requestedAt < 0 ? -1 : qMax<qint64>(0, monotonicMilliseconds() - requestedAt);
}

inline void resetCancelRequested()
{
    cancellationRequestedAtMs().store(-1, std::memory_order_release);
    cancelRequested().store(false, std::memory_order_release);
}

}   // namespace pdftool

#endif   // PDFTOOLCANCEL_H
