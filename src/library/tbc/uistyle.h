/******************************************************************************
 * uistyle.h
 * Shared Qt UI style helpers for ld-decode GUI tools
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef TBC_UISTYLE_H
#define TBC_UISTYLE_H

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QMargins>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleHints>
#include <QtGlobal>
#include <QWidget>
#include <QWindow>

namespace tbc::ui {
// Qt's own Fusion theme: the Fusion style plus a colour-scheme request, with
// Qt's palette unmodified. Qt::ColorScheme::Unknown follows the desktop, and
// Qt then tracks its changes. Needs Qt 6.10 for the full dark palette on Linux.
//
// Never set an application palette for the theme, here or elsewhere: an
// explicit palette stops later scheme changes from reaching widgets, and a
// partial one leaves every unset role (Light, Mid, most of the Disabled group)
// on the other scheme's values -- which is how disabled menu items once drew
// a white etched ghost. Never turn QGuiApplication's desktop-settings
// awareness off either: Qt then ignores the scheme request. Custom-painted colours come from palette
// roles (or theme_tokens) and are resolved again on QEvent::PaletteChange.
inline void applyFusionTheme(Qt::ColorScheme scheme)
{
    if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0) {
        QApplication::setStyle(QStringLiteral("Fusion"));
    }
    QGuiApplication::styleHints()->setColorScheme(scheme);
}

// Center a top-level sub-window over its parent widget (typically the main
// analyse window). Call this from a show-path override (e.g. setVisible or a
// show-event filter) so the placement is applied before the window manager
// maps the window.
//
// QWidget::move() positions the window *frame* on most platforms, while
// QWidget::width()/height() report the client area. Centering the client area
// without accounting for the frame margins leaves the window shifted right and
// down by the left/top frame margins, so we subtract them. The result is
// clamped to the available geometry of the window's screen.
inline void centerDialogOverParent(QWidget *dialog)
{
    if (!dialog) {
        return;
    }

    QWidget *parent = dialog->parentWidget();
    QRect referenceRect;
    if (parent) {
        referenceRect = parent->geometry();
    } else if (QScreen *screen = dialog->screen()) {
        referenceRect = screen->availableGeometry();
    } else {
        return;
    }

    // Force the native handle so the window-frame margins are known.
    if (!dialog->windowHandle()) {
        dialog->winId();
    }

    int leftMargin = 0;
    int topMargin = 0;
    if (QWindow *handle = dialog->windowHandle()) {
        const QMargins margins = handle->frameMargins();
        leftMargin = margins.left();
        topMargin = margins.top();
    }

    const int width = dialog->width();
    const int height = dialog->height();
    int x = referenceRect.x() + (referenceRect.width() - width) / 2 - leftMargin;
    int y = referenceRect.y() + (referenceRect.height() - height) / 2 - topMargin;

    if (QScreen *screen = dialog->screen()) {
        const QRect avail = screen->availableGeometry();
        const int maxX = avail.left() + qMax(0, avail.width() - width);
        const int maxY = avail.top() + qMax(0, avail.height() - height);
        x = qBound(avail.left(), x, maxX);
        y = qBound(avail.top(), y, maxY);
    }

    dialog->move(x, y);
}

// ---------------------------------------------------------------------------
// File dialogs
//
// One implementation for every GUI tool. This block previously existed as three
// byte-identical copies (tbc-analyse's exportdialog.cpp and
// metadataconversiondialog.cpp, plus tbc-export-metadata's
// metadataexportdialog.cpp), which meant three places to keep in step and three
// places deciding whether the dialog is native.
//
// Every platform gets its OS-native file browser except macOS, where the Qt
// dialog is used deliberately - keep that carve-out.
// ---------------------------------------------------------------------------

inline QWidget *dialogParentWidget(QWidget *widget)
{
    if (!widget) {
        return nullptr;
    }

    QWidget *window = widget->window();
    return window ? window : widget;
}

inline QStringList dialogNameFilters(const QString &filters)
{
    return filters.split(QStringLiteral(";;"), Qt::SkipEmptyParts);
}

inline void applyCommonFileDialogOptions(QFileDialog *dialog)
{
    if (!dialog) {
        return;
    }

    dialog->setOption(QFileDialog::DontResolveSymlinks, true);
}

inline QString runOpenFileDialog(QWidget *parent,
                                 const QString &title,
                                 const QString &startPath,
                                 const QString &filters)
{
    QFileDialog dialog(dialogParentWidget(parent), title, startPath);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(dialogNameFilters(filters));
    applyCommonFileDialogOptions(&dialog);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return QString();
    }
    return dialog.selectedFiles().constFirst();
}

// startPath may be either a directory to open in or a full suggested filename;
// when it names a file, the dialog pre-selects it. defaultSuffix is appended
// when the user types a name with no extension (empty = no default).
inline QString runSaveFileDialog(QWidget *parent,
                                 const QString &title,
                                 const QString &startPath,
                                 const QString &filters,
                                 const QString &defaultSuffix = QString())
{
    QFileDialog dialog(dialogParentWidget(parent), title, startPath);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(dialogNameFilters(filters));
    if (!defaultSuffix.isEmpty()) {
        dialog.setDefaultSuffix(defaultSuffix);
    }
    if (!startPath.isEmpty() && !QDir(startPath).exists()) {
        dialog.selectFile(startPath);
    }
    applyCommonFileDialogOptions(&dialog);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return QString();
    }
    return dialog.selectedFiles().constFirst();
}

inline QString runDirectoryDialog(QWidget *parent,
                                  const QString &title,
                                  const QString &startPath)
{
    QFileDialog dialog(dialogParentWidget(parent), title, startPath);
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);
    applyCommonFileDialogOptions(&dialog);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return QString();
    }
    return dialog.selectedFiles().constFirst();
}
} // namespace tbc::ui

#endif // TBC_UISTYLE_H
