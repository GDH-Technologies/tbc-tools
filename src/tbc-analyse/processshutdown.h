/******************************************************************************
 * processshutdown.h
 * tbc-analyse - stop a dialog's child processes before the dialog is destroyed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef PROCESSSHUTDOWN_H
#define PROCESSSHUTDOWN_H

#include <initializer_list>

class QObject;
class QProcess;

namespace ProcessShutdown {

// Stops *processes* for good while *owner* is being torn down.
//
// Every signal a process sends to *owner* is disconnected first, so nothing
// the process reports while it dies (errorOccurred, finished, readyRead...)
// reaches a half-destroyed owner. A QProcess member destroyed with a running
// child kills it and emits errorOccurred from its own destructor; with the
// owner's UI already deleted, ExportDialog::handleProcessError crashed there.
//
// A process still running is then stopped the way Cancel stops it: SIGINT
// first on Unix (tbc-video-export winds its own children down on it), and
// kill() if it is still running after *graceMs*. Null processes are skipped.
void stopForShutdown(QObject *owner, std::initializer_list<QProcess *> processes,
                     int graceMs = 3000);

} // namespace ProcessShutdown

#endif // PROCESSSHUTDOWN_H
