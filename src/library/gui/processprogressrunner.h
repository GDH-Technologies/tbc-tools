/******************************************************************************
 * processprogressrunner.h
 * tbc-tools - run a child process without freezing the GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef PROCESSPROGRESSRUNNER_H
#define PROCESSPROGRESSRUNNER_H

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>

class QWidget;

namespace ProcessProgressRunner {

struct Result {
    enum Status {
        Finished,       // ran to completion; read exitCode and exitStatus
        FailedToStart,  // the program could not be launched
        DidNotFinish,   // the wait itself failed
        Cancelled       // the user pressed Cancel
    };

    Status status = DidNotFinish;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QByteArray standardOutput;
    QByteArray standardError;
};

// Run program with arguments, collecting its output.
//
// With a parent widget, the caller's event loop keeps running, so the window
// still repaints and a modal progress dialog (shown once the run passes half a
// second, so a quick conversion never flashes one) offers a Cancel button that
// really stops the child. Without one, this is a plain blocking wait, which is
// what a non-GUI caller wants.
//
// On Linux the child is asked to die with its parent, so force-quitting the
// GUI cannot leave it running.
Result run(const QString &program,
           const QStringList &arguments,
           QWidget *parent,
           const QString &labelText);

} // namespace ProcessProgressRunner

#endif // PROCESSPROGRESSRUNNER_H
