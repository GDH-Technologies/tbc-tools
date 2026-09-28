/******************************************************************************
 * processprogressrunner.cpp
 * tbc-tools - run a child process without freezing the GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "processprogressrunner.h"

#include <QEventLoop>
#include <QObject>
#include <QProgressDialog>
#include <QTimer>
#include <QWidget>

#if defined(Q_OS_LINUX)
#include <sys/prctl.h>
#endif
#if defined(Q_OS_UNIX)
#include <csignal>
#endif

namespace {

// How long the child is given to honour terminate() before it is killed
constexpr int kTerminateGraceMs = 3000;

// A conversion shorter than this never gets a dialog
constexpr int kDialogDelayMs = 500;

void configureChildLifetime(QProcess &process)
{
#if defined(Q_OS_LINUX)
    // Ask the kernel to signal the child when this process dies, so a
    // force-quit of the GUI does not leave a converter running. Nothing in
    // process can defend against SIGKILL, which is exactly the case this
    // covers.
    process.setChildProcessModifier([] {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    });
#else
    Q_UNUSED(process);
#endif
}

void stopProcess(QProcess &process)
{
    if (process.state() == QProcess::NotRunning) return;

    process.terminate();
    if (!process.waitForFinished(kTerminateGraceMs)) {
        process.kill();
        process.waitForFinished(kTerminateGraceMs);
    }
}

// Split complete lines (ended by \n or \r) off the front of buffer
QStringList takeLines(QByteArray &buffer)
{
    QStringList lines;
    qsizetype start = 0;
    for (qsizetype i = 0; i < buffer.size(); ++i) {
        if (buffer.at(i) == '\n' || buffer.at(i) == '\r') {
            lines.append(QString::fromLocal8Bit(buffer.mid(start, i - start)));
            start = i + 1;
        }
    }
    buffer.remove(0, start);
    return lines;
}

} // namespace

namespace ProcessProgressRunner {

Result run(const QString &program,
           const QStringList &arguments,
           QWidget *parent,
           const QString &labelText,
           const Options &options)
{
    Result result;

    QProcess process;
    configureChildLifetime(process);
    if (!options.workingDirectory.isEmpty()) {
        process.setWorkingDirectory(options.workingDirectory);
    }
    if (!options.environment.isEmpty()) {
        process.setProcessEnvironment(options.environment);
    }
    if (options.onLine) {
        process.setProcessChannelMode(QProcess::MergedChannels);
    }
    process.start(program, arguments);

    if (!process.waitForStarted(-1)) {
        result.status = Result::FailedToStart;
        if (!options.onLine) {
            result.standardError = process.readAllStandardError();
        }
        return result;
    }

    // Line reporting: what the callback last asked the dialog to show
    QByteArray lineBuffer;
    int percent = -1;
    QString label = labelText;
    const auto reportLines = [&](const QStringList &lines) {
        for (const QString &line : lines) {
            options.onLine(line, &percent, &label);
        }
    };

    if (parent == nullptr) {
        // No GUI to keep alive
        if (!process.waitForFinished(-1)) {
            result.status = Result::DidNotFinish;
            stopProcess(process);
            return result;
        }
        result.status = Result::Finished;
        result.exitCode = process.exitCode();
        result.exitStatus = process.exitStatus();
        result.standardOutput = process.readAllStandardOutput();
        if (!options.onLine) {
            result.standardError = process.readAllStandardError();
        } else {
            lineBuffer = result.standardOutput;
            reportLines(takeLines(lineBuffer));
            if (!lineBuffer.isEmpty()) {
                reportLines({QString::fromLocal8Bit(lineBuffer)});
            }
        }
        return result;
    }

    // Drain the pipes as they fill, so a chatty child cannot block on a full
    // buffer while we sit in the event loop
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&]() {
        const QByteArray chunk = process.readAllStandardOutput();
        result.standardOutput += chunk;
        if (options.onLine) {
            lineBuffer += chunk;
            reportLines(takeLines(lineBuffer));
        }
    });
    if (!options.onLine) {
        QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&]() {
            result.standardError += process.readAllStandardError();
        });
    }

    // Indeterminate until the line callback reports a percentage: without one
    // there is no honest percentage to show
    QProgressDialog dialog(labelText, QObject::tr("Cancel"), 0, 0, parent);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.setMinimumDuration(kDialogDelayMs);

    QEventLoop loop;
    bool cancelled = false;

    QObject::connect(&process, &QProcess::finished, &loop,
                     [&loop](int, QProcess::ExitStatus) { loop.quit(); });
    QObject::connect(&process, &QProcess::errorOccurred, &loop,
                     [&loop](QProcess::ProcessError) { loop.quit(); });
    auto requestCancel = [&]() {
        if (cancelled) return;
        cancelled = true;
        dialog.setLabelText(QObject::tr("Cancelling..."));
#if defined(Q_OS_UNIX)
        if (options.interruptFirst && process.processId() > 0) {
            ::kill(static_cast<pid_t>(process.processId()), SIGINT);
        } else {
            process.terminate();
        }
#else
        process.terminate();
#endif
        QTimer::singleShot(kTerminateGraceMs, &process, [&process]() {
            if (process.state() != QProcess::NotRunning) process.kill();
        });
    };

    // The button click is the fast path. QProgressDialog::cancel() -- which is
    // what Escape and closing the window go through -- only sets wasCanceled()
    // and emits nothing, so poll for it too rather than hanging on a cancel
    // that never announced itself.
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, requestCancel);

    // The same tick applies what the line callback reported. Not from the
    // readyRead handler: QProgressDialog::setValue() on a modal dialog runs
    // processEvents(), which could re-enter that handler.
    QTimer cancelWatch;
    cancelWatch.setInterval(100);
    QObject::connect(&cancelWatch, &QTimer::timeout, &dialog, [&]() {
        if (dialog.wasCanceled()) {
            requestCancel();
            return;
        }
        if (cancelled) {
            return;
        }
        if (label != dialog.labelText()) {
            dialog.setLabelText(label);
        }
        if (percent >= 0) {
            if (dialog.maximum() != 100) {
                dialog.setRange(0, 100);
            }
            dialog.setValue(qBound(0, percent, 100));
        }
    });
    cancelWatch.start();

    if (process.state() != QProcess::NotRunning) {
        loop.exec();
    }
    cancelWatch.stop();

    dialog.hide();

    const QByteArray trailing = process.readAllStandardOutput();
    result.standardOutput += trailing;
    if (!options.onLine) {
        result.standardError += process.readAllStandardError();
    } else {
        lineBuffer += trailing;
        reportLines(takeLines(lineBuffer));
        if (!lineBuffer.isEmpty()) {
            reportLines({QString::fromLocal8Bit(lineBuffer)});
        }
    }

    if (cancelled) {
        stopProcess(process);
        result.status = Result::Cancelled;
        return result;
    }

    if (process.state() != QProcess::NotRunning) {
        stopProcess(process);
        result.status = Result::DidNotFinish;
        return result;
    }

    result.status = Result::Finished;
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    return result;
}

} // namespace ProcessProgressRunner
