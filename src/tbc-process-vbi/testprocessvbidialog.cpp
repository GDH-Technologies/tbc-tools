/******************************************************************************
 * testprocessvbidialog.cpp
 * tbc-process-vbi - processing dialog unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Reece Dodge
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdlib>
#include <functional>
#include <iostream>
#include <QtGlobal>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QThread>

#if !defined(Q_OS_WIN)
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTemporaryDir>

#include "processvbidialog.h"
#endif

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

// The dialog runs its own binary in CLI mode, which here is this test: with
// arguments it stands in for tbc-process-vbi, appending them to the log named
// by TBC_FAKE_VBI_LOG, then either exiting or (TBC_FAKE_VBI_SLOW) running
// until stopped
int runFakeTool(int argc, char *argv[])
{
    QFile log(QString::fromLocal8Bit(qgetenv("TBC_FAKE_VBI_LOG")));
    if (log.open(QIODevice::Append)) {
        QByteArrayList arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.append(argv[i]);
        }
        log.write(arguments.join(' ') + '\n');
    }
    if (!qEnvironmentVariableIsEmpty("TBC_FAKE_VBI_SLOW")) {
        QThread::sleep(30);
    }
    return 0;
}

#if !defined(Q_OS_WIN)
bool waitUntil(const std::function<bool()> &condition, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition()) {
        if (elapsed.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

QPushButton *button(const QDialog &dialog, const QString &text)
{
    for (QPushButton *candidate : dialog.findChildren<QPushButton *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    std::cerr << "FAIL no button " << text.toStdString() << "\n";
    std::exit(1);
}

QStringList logLines(const QString &logPath)
{
    QFile log(logPath);
    if (!log.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLocal8Bit(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// A run starts the tool once, with the input last and no --help probe first
void testRunInvokesItselfOnce(const QString &inputPath, const QString &logPath)
{
    std::cerr << "Testing a run\n";

    qunsetenv("TBC_FAKE_VBI_SLOW");
    ProcessVbiDialog dialog;
    dialog.setDefaultInputTbc(inputPath);
    CHECK(button(dialog, QStringLiteral("Run"))->isDefault());
    button(dialog, QStringLiteral("Run"))->click();
    CHECK(waitUntil([&dialog]() { return button(dialog, QStringLiteral("Run"))->isEnabled(); }, 20000));

    const QStringList lines = logLines(logPath);
    CHECK(lines.size() == 1);
    CHECK(!lines.first().contains(QLatin1String("--help")));
    CHECK(lines.first().endsWith(inputPath));
}

// Escape or the close button is refused while running; Stop ends the run
void testRejectRefusedWhileRunning(const QString &inputPath)
{
    std::cerr << "Testing reject() while running, and Stop\n";

    qputenv("TBC_FAKE_VBI_SLOW", "1");
    ProcessVbiDialog dialog;
    dialog.setDefaultInputTbc(inputPath);
    dialog.show();
    button(dialog, QStringLiteral("Run"))->click();
    QPushButton *stop = button(dialog, QStringLiteral("Stop"));
    CHECK(stop->isEnabled());

    dialog.reject();
    CHECK(dialog.isVisible());

    QElapsedTimer elapsed;
    elapsed.start();
    stop->click();
    CHECK(waitUntil([&dialog]() { return button(dialog, QStringLiteral("Run"))->isEnabled(); }, 10000));
    CHECK(elapsed.elapsed() < 10000);

    dialog.reject();
    CHECK(!dialog.isVisible());
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    if (argc > 1) {
        return runFakeTool(argc, argv);
    }

    // The dialog needs a QApplication, which needs a platform plugin:
    // offscreen, set by CMake. Not on Windows: there the self-hosted runner's
    // ctest (a service session, before windeployqt stages the plugins) hung in
    // the QApplication constructor.
#if !defined(Q_OS_WIN)
    QApplication application(argc, argv);

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString inputPath = QDir(directory.path()).filePath(QStringLiteral("capture.tbc"));
    {
        QFile input(inputPath);
        CHECK(input.open(QIODevice::WriteOnly));
    }
    const QString logPath = QDir(directory.path()).filePath(QStringLiteral("invocations.log"));
    qputenv("TBC_FAKE_VBI_LOG", logPath.toLocal8Bit());

    testRunInvokesItselfOnce(inputPath, logPath);
    testRejectRefusedWhileRunning(inputPath);
#endif

    std::cerr << "All process VBI dialog tests passed\n";
    return 0;
}
