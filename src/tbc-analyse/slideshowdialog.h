/******************************************************************************
 * slideshowdialog.h
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef SLIDESHOWDIALOG_H
#define SLIDESHOWDIALOG_H

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <atomic>
#include <functional>
#include <memory>

#include "configuration.h"
#include "slideshowextract.h"

class CropPreview;
class FrameSnapshotControls;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

// "Extract slideshow stills": scan a frame range for held photos, review the
// list, and save the ticked ones. The scan reads the .tbc on a worker thread
// and never touches TbcSource; saving renders frames, so MainWindow does it
// (saveRequested).
class SlideshowDialog : public QDialog
{
    Q_OBJECT

public:
    struct Source {
        TbcMetaData::VideoParameters videoParameters;
        QSize frameSize;
        qint32 frameCount = 0;
        qint32 currentFrame = 1;
        qint32 inPoint = -1; // the Export tab's In/Out selection; -1 = unset
        qint32 outPoint = -1;
        QImage currentFrameImage;
        QString defaultDirectory; // "<tape>_stills" next to the .tbc
    };
    // Builds the scan and capture input for frames first..last (MainWindow
    // owns TbcSource, which knows the field numbers and dropouts)
    using InputBuilder = std::function<SlideshowExtract::CaptureInput(qint32 first, qint32 last)>;

    SlideshowDialog(const SlideshowExtractOptions &settings, const Source &source, InputBuilder buildInput,
                    QWidget *parent = nullptr);
    ~SlideshowDialog() override;

    SlideshowExtractOptions currentSettings() const;
    void setStatus(const QString &text);

signals:
    void jumpRequested(qint32 frame);
    // input.options and input.scan.cropRect follow the current settings
    void saveRequested(const SlideshowExtract::CaptureInput &input, const QVector<SlideshowExtract::Hold> &holds,
                       const QString &directory);
    void settingsChanged(const SlideshowExtractOptions &settings);

private:
    struct ScanOutcome {
        SlideshowExtract::ScanResult scan;
        QVector<SlideshowExtract::Hold> holds;
        QVector<QImage> previews;
    };

    FrameSnapshot::Options currentOptions() const;
    void startScan();
    void scanFinished();
    void refreshRange();
    void refreshSaveButton();
    void showHold(QTreeWidgetItem *item);
    void browse();
    void save();

    Source source;
    InputBuilder buildInput;
    SlideshowExtract::CaptureInput scannedInput;
    QVector<SlideshowExtract::Hold> holds;
    QVector<QImage> previews;

    std::shared_ptr<std::atomic<bool>> scanCancel;
    std::shared_ptr<std::atomic<qint32>> scanProgress;
    QFutureWatcher<ScanOutcome> scanWatcher;
    QTimer *progressTimer = nullptr;

    CropPreview *preview = nullptr;
    FrameSnapshotControls *controls = nullptr;
    QSpinBox *fromSpin = nullptr;
    QSpinBox *toSpin = nullptr;
    QLabel *rangeLabel = nullptr;
    QPushButton *inOutButton = nullptr;
    QDoubleSpinBox *minHoldSpin = nullptr;
    QComboBox *captureCombo = nullptr;
    QSpinBox *radiusSpin = nullptr;
    QLineEdit *folderEdit = nullptr;
    QPushButton *scanButton = nullptr;
    QProgressBar *progressBar = nullptr;
    QLabel *statusLabel = nullptr;
    QTreeWidget *holdList = nullptr;
    QPushButton *saveButton = nullptr;
};

#endif // SLIDESHOWDIALOG_H
