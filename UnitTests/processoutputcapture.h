// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#pragma once

#include <QElapsedTimer>
#include <QProcess>

namespace test_support
{

inline bool waitForFinishedAndCapture(QProcess& process,
                                      int timeoutMs,
                                      QByteArray& standardOutput,
                                      QByteArray& standardError)
{
    const auto drainOutput = [&process, &standardOutput, &standardError]()
    {
        standardOutput.append(process.readAllStandardOutput());
        standardError.append(process.readAllStandardError());
    };

    QElapsedTimer timer;
    timer.start();

    while (process.state() != QProcess::NotRunning)
    {
        const int remainingMs = timeoutMs - static_cast<int>(timer.elapsed());
        if (remainingMs <= 0)
        {
            drainOutput();
            process.kill();
            process.waitForFinished(5000);
            drainOutput();
            return false;
        }

        // Drain both pipes periodically. On Windows, waiting for process exit
        // without reading can deadlock when a verbose child fills a pipe.
        process.waitForReadyRead(qMin(remainingMs, 100));
        drainOutput();
    }

    drainOutput();
    return process.error() != QProcess::FailedToStart;
}

}   // namespace test_support
