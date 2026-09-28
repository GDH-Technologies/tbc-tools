/******************************************************************************
 * testprocessprogressrunner.cpp
 * tbc-tools - ProcessProgressRunner unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Reece Dodge
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#if !defined(Q_OS_WIN)
#include <QApplication>
#include <QProgressDialog>
#include <QTimer>
#include <QWidget>
#endif
#if defined(Q_OS_UNIX)
#include <csignal>
#include <unistd.h>
#endif

#include "processprogressrunner.h"

// Release builds define NDEBUG, so a bare assert() would check nothing.
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "     \
                      << #condition << "\n";                                \
            std::exit(1);                                                   \
        }                                                                   \
    } while (false)

namespace {

// The child: this same binary, re-run with --child <mode>
int runChild(const char *mode)
{
    if (std::strcmp(mode, "lines") == 0) {
        // \n and \r both end a line; the last one is unterminated
        std::fputs("PCT 10\nsecond\rPCT 60\nlast", stdout);
        return 0;
    }
    if (std::strcmp(mode, "cwd") == 0) {
        std::fputs(QDir::currentPath().toLocal8Bit().constData(), stdout);
        return 0;
    }
    if (std::strcmp(mode, "env") == 0) {
        std::fputs(qgetenv("TBC_RUNNER_TEST").constData(), stdout);
        return 0;
    }
    if (std::strcmp(mode, "sleep") == 0) {
#if defined(Q_OS_UNIX)
        // A tool that stops cleanly on Ctrl+C
        std::signal(SIGINT, [](int) {
            const char message[] = "interrupted\n";
            [[maybe_unused]] const ssize_t written = ::write(STDOUT_FILENO, message, sizeof(message) - 1);
            ::_exit(3);
        });
#endif
        std::fputs("started\n", stdout);
        std::fflush(stdout);
        QThread::sleep(30);
        return 0;
    }
    return 2;
}

ProcessProgressRunner::Result runChildMode(const QString &mode, QWidget *parent,
                                           const ProcessProgressRunner::Options &options = {})
{
    return ProcessProgressRunner::run(QCoreApplication::applicationFilePath(),
                                      {QStringLiteral("--child"), mode}, parent,
                                      QStringLiteral("Testing"), options);
}

// Lines end at \n or \r, a final unterminated line is reported too, and the
// percentage a callback sets is what the next call starts from
void testLines(QWidget *parent)
{
    std::cerr << "Testing line splitting and progress (" << (parent ? "GUI" : "blocking") << ")\n";

    QStringList lines;
    QList<int> percentsSeen;
    ProcessProgressRunner::Options options;
    options.onLine = [&](const QString &line, int *percent, QString *label) {
        percentsSeen.append(*percent);
        lines.append(line);
        if (line.startsWith(QLatin1String("PCT "))) {
            *percent = line.mid(4).toInt();
            *label = line;
        }
    };
    const ProcessProgressRunner::Result result = runChildMode(QStringLiteral("lines"), parent, options);
    CHECK(result.status == ProcessProgressRunner::Result::Finished);
    CHECK(result.exitCode == 0);
    CHECK(lines == QStringList({QStringLiteral("PCT 10"), QStringLiteral("second"),
                                QStringLiteral("PCT 60"), QStringLiteral("last")}));
    CHECK(percentsSeen == QList<int>({-1, 10, 10, 60}));
}

void testWorkingDirectoryAndEnvironment(QWidget *parent)
{
    std::cerr << "Testing working directory and environment (" << (parent ? "GUI" : "blocking") << ")\n";

    QTemporaryDir directory;
    CHECK(directory.isValid());
    ProcessProgressRunner::Options options;
    options.workingDirectory = directory.path();
    ProcessProgressRunner::Result result = runChildMode(QStringLiteral("cwd"), parent, options);
    CHECK(result.status == ProcessProgressRunner::Result::Finished);
    CHECK(QDir(QString::fromLocal8Bit(result.standardOutput)).canonicalPath()
          == QDir(directory.path()).canonicalPath());

    options = {};
    options.environment = QProcessEnvironment::systemEnvironment();
    options.environment.insert(QStringLiteral("TBC_RUNNER_TEST"), QStringLiteral("from-options"));
    result = runChildMode(QStringLiteral("env"), parent, options);
    CHECK(result.status == ProcessProgressRunner::Result::Finished);
    CHECK(result.standardOutput == "from-options");
}

#if !defined(Q_OS_WIN)
// Cancel stops the child promptly. QProgressDialog::cancel() is what Escape and
// closing the window do; it emits nothing, so this also covers the polling.
// With interruptFirst the child gets SIGINT and can stop cleanly.
void testCancel(QWidget *parent, bool interruptFirst)
{
    std::cerr << "Testing cancel" << (interruptFirst ? " with SIGINT" : "") << "\n";

    QTimer::singleShot(800, parent, [parent]() {
        QProgressDialog *dialog = parent->findChild<QProgressDialog *>();
        CHECK(dialog != nullptr);
        dialog->cancel();
    });
    ProcessProgressRunner::Options options;
    options.interruptFirst = interruptFirst;
    QElapsedTimer elapsed;
    elapsed.start();
    const ProcessProgressRunner::Result result = runChildMode(QStringLiteral("sleep"), parent, options);
    CHECK(result.status == ProcessProgressRunner::Result::Cancelled);
    CHECK(elapsed.elapsed() < 10000);
    CHECK(result.standardOutput.contains("started"));
    CHECK(result.standardOutput.contains("interrupted") == interruptFirst);
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
        return runChild(argv[2]);
    }

    // Misuse QProcess warns rather than fails (reading stderr from merged
    // channels, say), so any QProcess warning fails the test
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
        if (type != QtDebugMsg && message.contains(QLatin1String("QProcess"))) {
            std::cerr << "FAIL unexpected warning: " << message.toStdString() << "\n";
            std::exit(1);
        }
    });

    // The GUI path needs a QApplication, which needs a platform plugin:
    // offscreen, set by CMake. Not on Windows: there the self-hosted runner's
    // ctest (a service session, before windeployqt stages the plugins) hung in
    // the QApplication constructor, so only the blocking path is tested.
#if !defined(Q_OS_WIN)
    QApplication application(argc, argv);
    testLines(nullptr);
    testWorkingDirectoryAndEnvironment(nullptr);

    QWidget parent;
    testLines(&parent);
    testWorkingDirectoryAndEnvironment(&parent);
    testCancel(&parent, false);
#if defined(Q_OS_UNIX)
    testCancel(&parent, true);
#endif
#else
    QCoreApplication application(argc, argv);
    testLines(nullptr);
    testWorkingDirectoryAndEnvironment(nullptr);
#endif

    std::cerr << "All process progress runner tests passed\n";
    return 0;
}
