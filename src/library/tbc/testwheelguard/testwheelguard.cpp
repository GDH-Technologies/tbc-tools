/******************************************************************************
 * testwheelguard.cpp
 * tbc-tools - app-wide wheel guard tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>
#include <QCoreApplication>

#if !defined(Q_OS_WIN)
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include "tbc/wheelguard.h"
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

#if !defined(Q_OS_WIN)
// A page taller than its scroll area whose first focusable widget is a
// dropdown, like the metadata editor's System row: Qt focuses it by itself when
// the window opens. Each case builds a fresh one, so focus and arming start clean.
struct Page
{
    QScrollArea area;
    QComboBox *system = new QComboBox;
    QSpinBox *gain = new QSpinBox;
    QComboBox *editable = new QComboBox;
    QDateTimeEdit *stamp = new QDateTimeEdit;
    QSlider *slider = new QSlider(Qt::Horizontal);
    QTabWidget *tabs = new QTabWidget;
    QPushButton *apply = new QPushButton(QStringLiteral("Apply"));

    Page()
    {
        auto *page = new QWidget;
        page->setMinimumHeight(2000);
        auto *layout = new QVBoxLayout(page);
        system->addItems({QStringLiteral("PAL"), QStringLiteral("NTSC"), QStringLiteral("PAL-M")});
        gain->setRange(0, 100);
        gain->setValue(50);
        editable->setEditable(true);
        editable->addItems({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
        // Mid-range, so a notch down moves whichever section is under the cursor
        stamp->setDateTime(QDateTime(QDate(2026, 6, 15), QTime(10, 30)));
        stamp->setCalendarPopup(true);
        slider->setRange(0, 100);
        slider->setValue(50);
        for (const QString &name : {QStringLiteral("VHS"), QStringLiteral("Hi8"), QStringLiteral("DV")}) {
            tabs->addTab(new QWidget, name);
        }
        for (QWidget *widget : {static_cast<QWidget *>(system), static_cast<QWidget *>(gain),
                                static_cast<QWidget *>(editable), static_cast<QWidget *>(stamp),
                                static_cast<QWidget *>(slider), static_cast<QWidget *>(tabs),
                                static_cast<QWidget *>(apply)}) {
            layout->addWidget(widget);
        }
        layout->addStretch();
        area.setWidget(page);
        area.resize(300, 250);
        area.show();
        area.activateWindow();
        CHECK(QTest::qWaitForWindowActive(&area));
    }

    int scrolled() const { return area.verticalScrollBar()->value(); }
};

QPoint probePoint(const QWidget *widget)
{
    return QPoint(10, widget->height() / 2);
}

// One notch down over the widget, sent through the window system: spontaneous,
// as a mouse delivers it. Only such a wheel reaches QApplication::notify's
// focus-on-wheel step, the one that made "has focus" useless as a test.
void wheelDown(Page &page, QWidget *widget)
{
    page.area.verticalScrollBar()->setValue(0);
    QTest::wheelEvent(page.area.windowHandle(), QPointF(widget->mapTo(&page.area, probePoint(widget))),
                      QPoint(0, -120));
    QCoreApplication::processEvents();
}

void click(QWidget *widget, QPoint position)
{
    QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, position);
    QCoreApplication::processEvents();
    if (QWidget *popup = QApplication::activePopupWidget()) {
        QTest::keyClick(popup, Qt::Key_Escape); // back out of a combo's list
        QCoreApplication::processEvents();
    }
}

void click(QWidget *widget)
{
    click(widget, probePoint(widget));
}

// Proves the harness can fail: before the guard, one hover notch changes a field
void testQtDefaultChangesAHoveredField()
{
    std::cerr << "Testing Qt's default\n";
    Page page;
    const int before = page.gain->value();
    wheelDown(page, page.gain);
    CHECK(page.gain->value() != before);
}

void testInstallIsIdempotent()
{
    std::cerr << "Testing install\n";
    tbc::ui::installWheelGuard();
    const QVariant first = qApp->property("tbcWheelGuard");
    CHECK(first.isValid());
    tbc::ui::installWheelGuard();
    CHECK(qApp->property("tbcWheelGuard") == first);
}

void testHoveredFieldsScrollThePage()
{
    std::cerr << "Testing hover over unfocused fields\n";
    for (int field = 0; field < 3; ++field) {
        Page page;
        QWidget *widget = field == 0 ? static_cast<QWidget *>(page.gain)
                        : field == 1 ? static_cast<QWidget *>(page.stamp)
                                     : static_cast<QWidget *>(page.slider);
        const QDateTime stamp = page.stamp->dateTime();
        wheelDown(page, widget);
        CHECK(page.gain->value() == 50);
        CHECK(page.stamp->dateTime() == stamp);
        CHECK(page.slider->value() == 50);
        CHECK(!widget->hasFocus());
        CHECK(page.scrolled() > 0);
    }
}

void testFocusPolicyDemotion()
{
    std::cerr << "Testing focus policies\n";
    Page page;
    CHECK(page.system->focusPolicy() == Qt::StrongFocus);
    CHECK(page.gain->focusPolicy() == Qt::StrongFocus);
    CHECK(page.stamp->focusPolicy() == Qt::StrongFocus);

    // A policy someone chose is left alone
    QComboBox chosen;
    chosen.setFocusPolicy(Qt::ClickFocus);
    chosen.ensurePolished();
    CHECK(chosen.focusPolicy() == Qt::ClickFocus);
}

// The window hands its first dropdown focus on open; that is not a click
void testAutoFocusedFirstFieldIsNotArmed()
{
    std::cerr << "Testing an auto-focused first field\n";
    Page page;
    CHECK(page.system->hasFocus());
    wheelDown(page, page.system);
    CHECK(page.system->currentText() == QLatin1String("PAL"));
    CHECK(page.scrolled() > 0);
}

void testClickArms()
{
    std::cerr << "Testing a click arms a field\n";
    {
        Page page;
        click(page.system); // focused already, so no FocusIn: the press arms it
        wheelDown(page, page.system);
        CHECK(page.system->currentText() == QLatin1String("NTSC"));
    }
    {
        Page page;
        click(page.gain);
        wheelDown(page, page.gain);
        CHECK(page.gain->value() == 49);
    }
    {
        Page page;
        const QDateTime before = page.stamp->dateTime();
        click(page.stamp);
        wheelDown(page, page.stamp);
        CHECK(page.stamp->dateTime() != before);
    }
    {
        Page page;
        click(page.slider);
        const int before = page.slider->value();
        wheelDown(page, page.slider);
        CHECK(page.slider->value() != before);
    }
    {
        Page page;
        QLineEdit *text = page.gain->findChild<QLineEdit *>();
        CHECK(text != nullptr);
        click(text, QPoint(5, text->height() / 2));
        wheelDown(page, page.gain);
        CHECK(page.gain->value() == 49);
    }
    {
        // The press the combo ignores climbs to its row; that must not disarm it
        Page page;
        click(page.editable);
        wheelDown(page, page.editable);
        CHECK(page.editable->currentText() == QLatin1String("two"));
    }
}

void testTabArms()
{
    std::cerr << "Testing Tab arms a field\n";
    Page page;
    QTest::keyClick(page.system, Qt::Key_Tab);
    QCoreApplication::processEvents();
    CHECK(page.gain->hasFocus());
    wheelDown(page, page.gain);
    CHECK(page.gain->value() == 49);
}

void testFocusFromCodeDoesNotArm()
{
    std::cerr << "Testing setFocus() does not arm\n";
    Page page;
    page.gain->setFocus();
    QCoreApplication::processEvents();
    CHECK(page.gain->hasFocus());
    wheelDown(page, page.gain);
    CHECK(page.gain->value() == 50);
}

void testClickingElsewhereDisarms()
{
    std::cerr << "Testing a click elsewhere disarms\n";
    Page page;
    click(page.gain);
    click(page.apply);
    wheelDown(page, page.gain);
    CHECK(page.gain->value() == 50);
    CHECK(page.scrolled() > 0);
}

void testTabBarNeverTakesTheWheel()
{
    std::cerr << "Testing tab bars\n";
    Page page;
    QTabBar *bar = page.tabs->tabBar();
    click(bar, bar->tabRect(1).center());
    CHECK(page.tabs->currentIndex() == 1); // a click still picks a tab
    wheelDown(page, bar);
    CHECK(page.tabs->currentIndex() == 1);
    CHECK(page.scrolled() > 0);
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    // Needs a QApplication, which needs a platform plugin: offscreen, set by
    // CMake. Not on Windows: there the self-hosted runner's ctest (a service
    // session) hung in the QApplication constructor.
#if !defined(Q_OS_WIN)
    QApplication application(argc, argv);
    testQtDefaultChangesAHoveredField();
    testInstallIsIdempotent();
    testHoveredFieldsScrollThePage();
    testFocusPolicyDemotion();
    testAutoFocusedFirstFieldIsNotArmed();
    testClickArms();
    testTabArms();
    testFocusFromCodeDoesNotArm();
    testClickingElsewhereDisarms();
    testTabBarNeverTakesTheWheel();
#else
    QCoreApplication application(argc, argv);
#endif

    std::cerr << "All wheel guard tests passed\n";
    return 0;
}
