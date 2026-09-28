/******************************************************************************
 * testmetadataconversiondialog.cpp
 * tbc-metadata-converter - conversion dialog unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Reece Dodge
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>
#include <QtGlobal>

#if !defined(Q_OS_WIN)
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>

#include "metadataconversiondialog.h"
#include "metadataconverterutil.h"
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

// Convert needs an input whose direction is known and an output; a given
// output (--output-json/--output-sqlite) overrides the suggested one
void testConvertFollowsTheInputs()
{
    std::cerr << "Testing Convert enablement and the output prefill\n";

    MetadataConversionDialog dialog;
    QPushButton *convert = dialog.findChild<QPushButton *>(QStringLiteral("convertButton"));
    QLineEdit *output = dialog.findChild<QLineEdit *>(QStringLiteral("outputLineEdit"));
    CHECK(convert != nullptr && output != nullptr);
    CHECK(convert->isDefault());
    CHECK(!convert->isEnabled());

    dialog.setDefaultInput(QStringLiteral("/tmp/capture.tbc.json"));
    CHECK(!output->text().isEmpty());
    CHECK(convert->isEnabled());

    const QString chosenOutput = MetadataConverterUtil::normalizePathForCurrentPlatform(
        QStringLiteral("/tmp/elsewhere/capture.tbc.db"));
    dialog.setDefaultOutput(chosenOutput);
    CHECK(output->text() == chosenOutput);

    output->clear();
    CHECK(!convert->isEnabled());

    output->setText(chosenOutput);
    dialog.setDefaultInput(QStringLiteral("/tmp/capture.txt"));
    CHECK(!convert->isEnabled());
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
    testConvertFollowsTheInputs();
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif

    std::cerr << "All conversion dialog tests passed\n";
    return 0;
}
