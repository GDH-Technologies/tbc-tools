/******************************************************************************
 * testconverterdialog.cpp
 * ld-lds-converter - converter dialog unit tests
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

#if !defined(Q_OS_WIN)
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>

#include "converterdialog.h"
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

#if !defined(Q_OS_WIN)
namespace {

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

QPushButton *button(const QDialog &dialog, const char *objectName)
{
    QPushButton *found = dialog.findChild<QPushButton *>(QLatin1String(objectName));
    CHECK(found != nullptr);
    return found;
}

// Nothing to convert, nothing to remove or clear; and no button is a default,
// because Enter in the input field adds to the queue
void testControlsFollowTheQueue()
{
    std::cerr << "Testing control enablement\n";

    ConverterDialog dialog;
    for (QPushButton *each : dialog.findChildren<QPushButton *>()) {
        CHECK(!each->autoDefault() && !each->isDefault());
    }
    CHECK(!button(dialog, "convertButton")->isEnabled());
    CHECK(!button(dialog, "removeQueuedButton")->isEnabled());
    CHECK(!button(dialog, "clearQueuedButton")->isEnabled());

    dialog.findChild<QLineEdit *>(QStringLiteral("inputLineEdit"))->setText(QStringLiteral("/tmp/capture.lds"));
    CHECK(button(dialog, "convertButton")->isEnabled());
    CHECK(!button(dialog, "clearQueuedButton")->isEnabled());

    dialog.setDefaultInputs({QStringLiteral("/tmp/a.lds"), QStringLiteral("/tmp/b.lds")});
    CHECK(button(dialog, "clearQueuedButton")->isEnabled());
    CHECK(!button(dialog, "removeQueuedButton")->isEnabled());
}

// Escape or the close button mid-conversion stops the run and closes once it
// has wound down, rather than closing over it or being ignored
void testRejectWhileConvertingStopsThenCloses()
{
    std::cerr << "Testing reject() during a conversion\n";

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString inputPath = QDir(directory.path()).filePath(QStringLiteral("capture.lds"));
    {
        QFile input(inputPath);
        CHECK(input.open(QIODevice::WriteOnly));
        CHECK(input.write(QByteArray(20000000, '\x55')) == 20000000);
    }

    ConverterDialog dialog;
    dialog.setDefaultInput(inputPath);
    dialog.setDefaultOutput(QDir(directory.path()).filePath(QStringLiteral("capture.flac")));
    dialog.show();
    button(dialog, "convertButton")->click();
    CHECK(!button(dialog, "convertButton")->isEnabled());

    dialog.reject();
    CHECK(dialog.isVisible());
    CHECK(waitUntil([&dialog]() { return !dialog.isVisible(); }, 30000));
    CHECK(button(dialog, "convertButton")->isEnabled());
}

} // namespace
#endif

int main(int argc, char *argv[])
{
    // The dialog needs a QApplication, which needs a platform plugin:
    // offscreen, set by CMake. Not on Windows: there the self-hosted runner's
    // ctest (a service session, before windeployqt stages the plugins) hung in
    // the QApplication constructor.
#if !defined(Q_OS_WIN)
    QApplication application(argc, argv);
    testControlsFollowTheQueue();
    testRejectWhileConvertingStopsThenCloses();
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif

    std::cerr << "All converter dialog tests passed\n";
    return 0;
}
