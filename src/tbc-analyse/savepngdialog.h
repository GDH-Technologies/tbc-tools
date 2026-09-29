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
#include <QGroupBox>
#include <QImage>
#include <functional>

#include "framesnapshot.h"

class QButtonGroup;
class QComboBox;
class QLabel;
class QRadioButton;
class QSlider;
class QSpinBox;

// A frame, scaled to fit and shown at the chosen pixel aspect, with the
// output rectangle drawn over it. Dragging draws a custom rectangle.
class CropPreview : public QWidget
{
public:
    explicit CropPreview(QWidget *parent = nullptr);

    // The image may be a scaled copy of a frame of frameSize; rectangles are
    // always in frame coordinates
    void setImage(const QImage &image, const QSize &frameSize);
    void setOverlay(const QRect &rect, double aspect);

    std::function<void(const QRect &)> onRectDrawn;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QRectF imageTarget() const;
    QRectF toWidget(const QRect &frameRect) const;
    QPoint toFrame(const QPointF &point) const;

    QImage image;
    QSize frameSize;
    QRect overlayRect;
    double pixelAspect = 1.0;
    bool dragging = false;
    QPoint dragStart;
    QPoint dragEnd;
};

// The Framing, Aspect ratio and Resize groups, shared by "Save frame as PNG"
// and "Extract slideshow stills". They drive a CropPreview (a drawn rectangle
// becomes the custom framing) and edit those fields of an Options; the still
// picture fields pass through untouched.
class FrameSnapshotControls : public QWidget
{
    Q_OBJECT

public:
    // middleGroup, if given, is laid out between Framing and Aspect ratio
    FrameSnapshotControls(const TbcMetaData::VideoParameters &videoParameters, const QSize &frameSize,
                          CropPreview *preview, QWidget *middleGroup = nullptr, QWidget *parent = nullptr);

    void setOptions(const FrameSnapshot::Options &options);
    void applyTo(FrameSnapshot::Options &options) const;

signals:
    void changed();

private:
    void refresh();

    TbcMetaData::VideoParameters videoParameters;
    QSize frameSize;
    QRect customRect;

    CropPreview *preview = nullptr;
    QButtonGroup *framingGroup = nullptr;
    QRadioButton *activeRadio = nullptr;
    QRadioButton *customRadio = nullptr;
    QSpinBox *marginLeftSpin = nullptr;
    QSpinBox *marginTopSpin = nullptr;
    QSpinBox *marginRightSpin = nullptr;
    QSpinBox *marginBottomSpin = nullptr;
    QButtonGroup *aspectGroup = nullptr;
    QComboBox *upscaleCombo = nullptr;
    QComboBox *methodCombo = nullptr;
    QLabel *customLabel = nullptr;
    QLabel *outputLabel = nullptr;
};

// How a held still is captured, as radio buttons with the search window below
// them: a spinbox and a slider in step. The window's label follows the mode,
// "Averaging window" for Average and "Search window" otherwise; it is
// disabled for StillMode::Off. Shared by both dialogs, each with its modes.
class StillModeControls : public QGroupBox
{
    Q_OBJECT

public:
    struct Mode {
        FrameSnapshot::StillMode mode;
        QString label;
        QString toolTip;
    };
    StillModeControls(const QString &title, const QVector<Mode> &modes, QWidget *parent = nullptr);

    FrameSnapshot::StillMode mode() const;
    qint32 window() const;
    void setMode(FrameSnapshot::StillMode mode);
    void setWindow(qint32 frames);

signals:
    void changed();

private:
    void refresh();

    QButtonGroup *modeGroup = nullptr;
    QLabel *windowLabel = nullptr;
    QSpinBox *windowSpin = nullptr;
    QSlider *windowSlider = nullptr;
};

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

    FrameSnapshotControls *controls = nullptr;
    StillModeControls *stillControls = nullptr;
};

#endif // SAVEPNGDIALOG_H
