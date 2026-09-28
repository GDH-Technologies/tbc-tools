/******************************************************************************
 * testaudioalignmentdialog.cpp
 * tbc-audio-align - alignment dialog unit tests
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
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>

#include "audioalignmentdialog.h"
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

// Each RF preset carries its rate; any other rate selects Custom (data 0)
// with the rate in the spin box
void testRfPresetsRoundTrip()
{
    std::cerr << "Testing the RF sample rate presets\n";

    AudioAlignmentDialog dialog;
    QComboBox *presets = dialog.findChild<QComboBox *>(QStringLiteral("rfVideoRatePresetComboBox"));
    QSpinBox *custom = dialog.findChild<QSpinBox *>(QStringLiteral("rfVideoSampleRateCustomSpinBox"));
    CHECK(presets != nullptr && custom != nullptr);
    CHECK(presets->currentData().toUInt() == 40000000u);

    for (const quint32 rate : {40000000u, 20000000u, 16000000u}) {
        dialog.setDefaultRfVideoSampleRate(rate);
        CHECK(presets->currentData().toUInt() == rate);
        CHECK(custom->value() == static_cast<int>(rate));
        CHECK(!custom->isEnabled());
    }

    dialog.setDefaultRfVideoSampleRate(28636360);
    CHECK(presets->currentData().toUInt() == 0u);
    CHECK(custom->value() == 28636360);
    CHECK(custom->isEnabled());

    // Choosing a preset by hand puts its rate in the spin box
    presets->setCurrentIndex(presets->findData(20000000u));
    CHECK(custom->value() == 20000000);
    CHECK(!custom->isEnabled());
}

// Align is the default button and nothing else is auto-default, so Enter in a
// field aligns instead of opening a file dialog
void testDefaultButton()
{
    std::cerr << "Testing the default button\n";

    AudioAlignmentDialog dialog;
    for (QPushButton *each : dialog.findChildren<QPushButton *>()) {
        if (each->objectName() == QLatin1String("alignButton")) {
            CHECK(each->isDefault());
        } else {
            CHECK(!each->autoDefault());
        }
    }
}

// With nothing running, Escape closes it
void testRejectWhenIdle()
{
    std::cerr << "Testing reject() when idle\n";

    AudioAlignmentDialog dialog;
    dialog.show();
    dialog.reject();
    CHECK(!dialog.isVisible());
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
    testRfPresetsRoundTrip();
    testDefaultButton();
    testRejectWhenIdle();
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif

    std::cerr << "All alignment dialog tests passed\n";
    return 0;
}
