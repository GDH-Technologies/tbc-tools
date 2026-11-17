/******************************************************************************
 * processshutdown.cpp
 * tbc-analyse - stop a dialog's child processes before the dialog is destroyed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "processshutdown.h"

#include <QObject>
#include <QProcess>

#if defined(Q_OS_UNIX)
#include <csignal>
#include <sys/types.h>
#endif

namespace ProcessShutdown {

void stopForShutdown(QObject *owner, std::initializer_list<QProcess *> processes, int graceMs)
{
    for (QProcess *process : processes) {
        if (!process) {
            continue;
        }
        QObject::disconnect(process, nullptr, owner, nullptr);
        if (process->state() == QProcess::NotRunning) {
            continue;
        }
#if defined(Q_OS_UNIX)
        const qint64 pid = process->processId();
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), SIGINT) == 0
            && process->waitForFinished(graceMs)) {
            continue;
        }
#endif
        process->kill();
        process->waitForFinished(graceMs);
    }
}

} // namespace ProcessShutdown
