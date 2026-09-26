/******************************************************************************
 * savepngdialog.h
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef SAVEPNGDIALOG_H
#define SAVEPNGDIALOG_H

#include <QDialog>
#include <QImage>

#include "framesnapshot.h"

class QButtonGroup;
class QComboBox;
class QLabel;
class QRadioButton;
class QSpinBox;
class CropPreview;

// Options for "Save frame as PNG": framing (with a rectangle dragged on the
// preview), aspect, best-frame search and upscaling. The quick Ctrl+F save
// skips this dialog and reuses what was accepted here last.
class SavePngDialog : public QDialog
{
    Q_OBJECT

public:
    SavePngDialog(const FrameSnapshot::Options &current, const QImage &frameImage,
                  const TbcMetaData::VideoParameters &videoParameters, bool frameView,
                  QWidget *parent = nullptr);

    FrameSnapshot::Options selectedOptions() const;

private:
    void setControls(const FrameSnapshot::Options &options);
    void refresh();

    QImage frameImage;
    TbcMetaData::VideoParameters videoParameters;
    QRect customRect;

    CropPreview *preview = nullptr;
    QButtonGroup *framingGroup = nullptr;
    QRadioButton *fullRadio = nullptr;
    QRadioButton *activeRadio = nullptr;
    QRadioButton *customRadio = nullptr;
    QSpinBox *marginLeftSpin = nullptr;
    QSpinBox *marginTopSpin = nullptr;
    QSpinBox *marginRightSpin = nullptr;
    QSpinBox *marginBottomSpin = nullptr;
    QComboBox *aspectCombo = nullptr;
    QComboBox *stillCombo = nullptr;
    QSpinBox *radiusSpin = nullptr;
    QComboBox *upscaleCombo = nullptr;
    QComboBox *methodCombo = nullptr;
    QLabel *customLabel = nullptr;
    QLabel *outputLabel = nullptr;
};

#endif // SAVEPNGDIALOG_H
