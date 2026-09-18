/******************************************************************************
 * headlessalign.h
 * tbc-audio-align - Headless (no GUI) audio alignment entry point
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef HEADLESSALIGN_H
#define HEADLESSALIGN_H

namespace HeadlessAlign {

// Exit codes returned by run(). Automation (e.g. a worker queue) keys off
// these instead of checking whether an output file appeared.
enum ExitCode {
    Ok = 0,
    AlignFailed = 1,
    UsageError = 2,
    RuntimeUnavailable = 3,
};

// True when argv carries --headless. Checked before any QApplication exists,
// so a headless run never needs a display.
bool requested(int argc, char *argv[]);

// Parses the headless options, runs AudioAlignmentUtil::runStreamAlign on a
// QCoreApplication and returns an ExitCode. Progress is written to stderr as
// "Progress: NN% message" lines; with --result-json a JSON summary is written
// to the given path ("-" = stdout).
int run(int argc, char *argv[]);

} // namespace HeadlessAlign

#endif // HEADLESSALIGN_H
