/******************************************************************************
 * testmetadataexportdialog.cpp
 * tbc-export-metadata - export dialog unit tests
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
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>

#include "metadataexportdialog.h"
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

// Its own binary (or one beside it) has every option, so nothing is probed
void testOwnToolIsNotProbed()
{
    std::cerr << "Testing that the dialog's own tool is not probed\n";

    MetadataExportDialog dialog;
    dialog.setExportExecutablePath(QCoreApplication::applicationFilePath());
    CHECK(dialog.findChild<QProcess *>() == nullptr);
    CHECK(dialog.findChild<QCheckBox *>(QStringLiteral("userMarkersTxtCheckBox"))->isEnabled());
    CHECK(dialog.findChild<QCheckBox *>(QStringLiteral("userMarkersCsvCheckBox"))->isEnabled());
}

// Another tool is probed asynchronously; one that can't run supports nothing
// optional, and a choice made for it is cleared
void testForeignToolIsProbed()
{
    std::cerr << "Testing the probe of another export tool\n";

    QTemporaryDir directory;
    CHECK(directory.isValid());
    MetadataExportDialog dialog;
    QCheckBox *markersTxt = dialog.findChild<QCheckBox *>(QStringLiteral("userMarkersTxtCheckBox"));
    markersTxt->setChecked(true);
    dialog.setExportExecutablePath(QDir(directory.path()).filePath(QStringLiteral("tbc-export-metadata")));
    CHECK(waitUntil([&dialog]() { return dialog.findChild<QProcess *>() == nullptr; }, 10000));
    CHECK(waitUntil([markersTxt]() { return markersTxt->toolTip().startsWith(QLatin1String("Unavailable")); }, 10000));
    CHECK(!markersTxt->isEnabled());
    CHECK(!markersTxt->isChecked());
}

// Start and length are spin boxes whose 0 means the whole source
void testRangeSpinBoxes()
{
    std::cerr << "Testing the FFMETADATA range spin boxes\n";

    MetadataExportDialog dialog;
    QSpinBox *start = dialog.findChild<QSpinBox *>(QStringLiteral("ffmetadataStartSpinBox"));
    QSpinBox *length = dialog.findChild<QSpinBox *>(QStringLiteral("ffmetadataLengthSpinBox"));
    CHECK(start != nullptr && length != nullptr);

    MetadataExportDialog::InitialOptions options;
    options.exportFfmetadata = true;
    dialog.setInitialOptions(options);
    CHECK(start->value() == 0 && start->text() == start->specialValueText());
    CHECK(length->value() == 0 && length->text() == length->specialValueText());
    CHECK(start->isEnabled() && length->isEnabled());

    options.ffmetadataStart = 25;
    options.ffmetadataLength = 100;
    dialog.setInitialOptions(options);
    CHECK(start->value() == 25);
    CHECK(length->value() == 100);

    CHECK(dialog.findChild<QPushButton *>(QStringLiteral("exportButton"))->isDefault());
    CHECK(!dialog.findChild<QPushButton *>(QStringLiteral("inputBrowseButton"))->autoDefault());
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
    testOwnToolIsNotProbed();
    testForeignToolIsProbed();
    testRangeSpinBoxes();
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif

    std::cerr << "All export dialog tests passed\n";
    return 0;
}
