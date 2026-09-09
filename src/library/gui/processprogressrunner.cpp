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
#include <csignal>
#include <sys/prctl.h>
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

} // namespace

namespace ProcessProgressRunner {

Result run(const QString &program,
           const QStringList &arguments,
           QWidget *parent,
           const QString &labelText)
{
    Result result;

    QProcess process;
    configureChildLifetime(process);
    process.start(program, arguments);

    if (!process.waitForStarted(-1)) {
        result.status = Result::FailedToStart;
        result.standardError = process.readAllStandardError();
        return result;
    }

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
        result.standardError = process.readAllStandardError();
        return result;
    }

    // Drain the pipes as they fill, so a chatty child cannot block on a full
    // buffer while we sit in the event loop
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&]() {
        result.standardOutput += process.readAllStandardOutput();
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&]() {
        result.standardError += process.readAllStandardError();
    });

    // An indeterminate dialog: the converter reports no progress of its own,
    // so there is no honest percentage to show
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
        process.terminate();
        QTimer::singleShot(kTerminateGraceMs, &process, [&process]() {
            if (process.state() != QProcess::NotRunning) process.kill();
        });
    };

    // The button click is the fast path. QProgressDialog::cancel() -- which is
    // what Escape and closing the window go through -- only sets wasCanceled()
    // and emits nothing, so poll for it too rather than hanging on a cancel
    // that never announced itself.
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, requestCancel);

    QTimer cancelWatch;
    cancelWatch.setInterval(100);
    QObject::connect(&cancelWatch, &QTimer::timeout, &dialog, [&]() {
        if (dialog.wasCanceled()) requestCancel();
    });
    cancelWatch.start();

    if (process.state() != QProcess::NotRunning) {
        loop.exec();
    }
    cancelWatch.stop();

    dialog.hide();

    result.standardOutput += process.readAllStandardOutput();
    result.standardError += process.readAllStandardError();

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
