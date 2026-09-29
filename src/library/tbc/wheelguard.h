/******************************************************************************
 * wheelguard.h
 * App-wide guard: the mouse wheel changes a field only once it is clicked into
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef TBC_WHEELGUARD_H
#define TBC_WHEELGUARD_H

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QObject>
#include <QPointer>
#include <QSlider>
#include <QTabBar>
#include <QVariant>
#include <QWidget>

namespace tbc::ui {
// A wheel over a dropdown, spin box (date/time edits included) or slider that
// the operator has not clicked or tabbed into scrolls whatever is behind it
// instead of changing the value; a tab bar never takes the wheel, so a tab
// changes only by clicking it or by keyboard. One application event filter does
// it for every widget in the program. Two Qt behaviours shape it:
//
// - For a real wheel, QApplication::notify() focuses a Qt::WheelFocus widget
//   (Qt's default for combo and spin boxes) *before* any event filter sees the
//   wheel. So each such field is demoted to Qt::StrongFocus when Qt polishes
//   it, which keeps click and Tab focus and drops only focus-by-wheel.
// - Focus alone is not the operator's choice: Qt focuses a window's first field
//   by itself when the window opens, and code calls setFocus(). So a field is
//   armed only by a mouse press on it (which also covers one focused already,
//   which gets no FocusIn) or by Tab, Shift+Tab or a shortcut landing on it, and
//   takes the wheel only while it is focused and armed. Focus returning from its
//   own popup or from another window keeps the arming; focus going to any other
//   widget drops it. A field a click cannot focus is armed by the press alone:
//   on macOS Qt gives a non-editable combo box Qt::TabFocus, so a click never
//   focuses it.
//
// A refused wheel is ignored and filtered: Qt then carries a real (spontaneous)
// wheel on to the parent widgets, and the scroll area behind scrolls.
class WheelGuard : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Polish:
            if (QWidget *field = asField(watched); field && field->focusPolicy() == Qt::WheelFocus) {
                field->setFocusPolicy(Qt::StrongFocus); // a policy someone chose is left alone
            }
            break;
        case QEvent::MouseButtonPress:
            // Only ever arms: a press the combo ignores climbs to its row, and
            // that must not undo the click. Focus moving away is what disarms.
            if (watched->isWidgetType()) {
                if (QWidget *field = fieldOf(static_cast<QWidget *>(watched))) {
                    armed = field;
                }
            }
            break;
        case QEvent::FocusIn:
            if (watched->isWidgetType()) {
                noteFocus(static_cast<QWidget *>(watched), static_cast<QFocusEvent *>(event)->reason());
            }
            break;
        case QEvent::Wheel:
            if (qobject_cast<QTabBar *>(watched)) {
                event->ignore();
                return true;
            }
            if (QWidget *field = asField(watched); field && !isArmed(field)) {
                event->ignore();
                return true;
            }
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    // QSlider, not QAbstractSlider: a scroll bar must keep its wheel
    static QWidget *asField(QObject *object)
    {
        if (qobject_cast<QComboBox *>(object) || qobject_cast<QAbstractSpinBox *>(object)
            || qobject_cast<QSlider *>(object)) {
            return static_cast<QWidget *>(object);
        }
        return nullptr;
    }

    // The field a widget is, or is part of (a spin box's text, say)
    static QWidget *fieldOf(QWidget *widget)
    {
        for (; widget; widget = widget->parentWidget()) {
            if (QWidget *field = asField(widget)) {
                return field;
            }
            if (widget->isWindow()) {
                break;
            }
        }
        return nullptr;
    }

    bool isArmed(const QWidget *field) const
    {
        return field == armed && (field->hasFocus() || !(field->focusPolicy() & Qt::ClickFocus));
    }

    void noteFocus(QWidget *widget, Qt::FocusReason reason)
    {
        if (widget->window()->windowType() == Qt::Popup) {
            return; // a combo's list taking focus while it is open
        }
        switch (reason) {
        case Qt::TabFocusReason:
        case Qt::BacktabFocusReason:
        case Qt::ShortcutFocusReason:
            armed = widget;
            break;
        case Qt::PopupFocusReason:
        case Qt::ActiveWindowFocusReason:
            break; // focus coming back: arming unchanged
        default:
            // A click elsewhere, a window opening, or code calling setFocus()
            if (widget != armed) {
                armed = nullptr;
            }
            break;
        }
    }

    QPointer<QWidget> armed;
};

// Install the guard on the application once. Call it before building any
// window: Qt polishes a widget once, often while its window is being built, and
// a field polished before the guard exists keeps Qt::WheelFocus.
inline void installWheelGuard()
{
    QCoreApplication *application = QCoreApplication::instance();
    if (!application || application->property("tbcWheelGuard").isValid()) {
        return;
    }
    application->installEventFilter(new WheelGuard(application));
    application->setProperty("tbcWheelGuard", true);
}
} // namespace tbc::ui

#endif // TBC_WHEELGUARD_H
