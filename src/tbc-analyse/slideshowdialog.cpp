/******************************************************************************
 * slideshowdialog.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "slideshowdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "savepngdialog.h"
#include "tbc/uistyle.h"

namespace {
// Review list columns
enum Column { StillColumn, StartColumn, LengthColumn, FramesColumn, KindColumn };
constexpr qint32 PREVIEW_WIDTH = 320;
constexpr qint32 THUMBNAIL_HEIGHT = 72;
} // namespace

SlideshowDialog::SlideshowDialog(const SlideshowExtractOptions &settings, const Source &source,
                                 InputBuilder buildInput, QWidget *parent)
    : QDialog(parent), source(source), buildInput(std::move(buildInput))
{
    setWindowTitle(tr("Extract slideshow stills"));

    auto *mainLayout = new QHBoxLayout(this);

    // Left: the picture and the review list
    auto *reviewLayout = new QVBoxLayout();
    mainLayout->addLayout(reviewLayout, 1);
    preview = new CropPreview(this);
    preview->setImage(source.currentFrameImage, source.frameSize);
    preview->setToolTip(tr("Drag a rectangle to frame the picture yourself."));
    reviewLayout->addWidget(preview, 2);

    holdList = new QTreeWidget(this);
    holdList->setHeaderLabels({tr("Still"), tr("Start"), tr("Length"), tr("Frames"), tr("Kind")});
    holdList->setRootIsDecorated(false);
    holdList->setUniformRowHeights(true);
    holdList->setIconSize(QSize(THUMBNAIL_HEIGHT * 2, THUMBNAIL_HEIGHT));
    holdList->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    holdList->setToolTip(tr("Untick anything that is not a photo. Double-click to show the frame in the viewer."));
    reviewLayout->addWidget(holdList, 3);

    auto *listButtons = new QHBoxLayout();
    auto *allButton = new QPushButton(tr("Tick all"), this);
    auto *noneButton = new QPushButton(tr("Untick all"), this);
    for (QPushButton *button : {allButton, noneButton}) {
        button->setAutoDefault(false);
        listButtons->addWidget(button);
    }
    listButtons->addStretch(1);
    reviewLayout->addLayout(listButtons);

    // Right: the settings, scrolling on a small screen
    auto *settingsWidget = new QWidget(this);
    auto *settingsLayout = new QVBoxLayout(settingsWidget);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidget(settingsWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *rightLayout = new QVBoxLayout();
    rightLayout->addWidget(scrollArea, 1);
    mainLayout->addLayout(rightLayout);

    // Range
    auto *rangeBox = new QGroupBox(tr("Frames to scan"), settingsWidget);
    auto *rangeLayout = new QFormLayout(rangeBox);
    fromSpin = new QSpinBox(rangeBox);
    toSpin = new QSpinBox(rangeBox);
    for (QSpinBox *spin : {fromSpin, toSpin}) spin->setRange(1, std::max(1, source.frameCount));
    const bool hasInOut = source.inPoint >= 1 && source.outPoint >= source.inPoint;
    fromSpin->setValue(hasInOut ? source.inPoint : source.currentFrame);
    toSpin->setValue(hasInOut ? source.outPoint : source.frameCount);
    rangeLayout->addRow(tr("From frame:"), fromSpin);
    rangeLayout->addRow(tr("To frame:"), toSpin);
    rangeLabel = new QLabel(rangeBox);
    rangeLayout->addRow(rangeLabel);
    auto *rangeButtons = new QHBoxLayout();
    auto *wholeButton = new QPushButton(tr("Whole tape"), rangeBox);
    inOutButton = new QPushButton(tr("In/Out selection"), rangeBox);
    inOutButton->setEnabled(hasInOut);
    inOutButton->setToolTip(hasInOut ? tr("Frames %1-%2, set on the Export tab").arg(source.inPoint).arg(source.outPoint)
                                     : tr("No In/Out selection is set (Export tab, or [ and ] in the viewer)."));
    for (QPushButton *button : {wholeButton, inOutButton}) {
        button->setAutoDefault(false);
        rangeButtons->addWidget(button);
    }
    rangeLayout->addRow(rangeButtons);
    settingsLayout->addWidget(rangeBox);

    // Detection
    auto *detectionBox = new QGroupBox(tr("Detection"), settingsWidget);
    auto *detectionLayout = new QFormLayout(detectionBox);
    minHoldSpin = new QDoubleSpinBox(detectionBox);
    minHoldSpin->setRange(0.1, 60.0);
    minHoldSpin->setSingleStep(0.1);
    minHoldSpin->setDecimals(1);
    minHoldSpin->setSuffix(tr(" s"));
    minHoldSpin->setValue(settings.minHoldSeconds);
    minHoldSpin->setToolTip(tr("A picture held for less than this is not listed: dissolves, flashes and "
                               "brief movement."));
    detectionLayout->addRow(tr("Shortest photo:"), minHoldSpin);
    auto *detectionNote = new QLabel(tr("Black and blank stretches are skipped. Slow pans, zooms and stretches "
                                        "that keep breaking up are listed unticked, and saved as their cleanest "
                                        "frame."), detectionBox);
    detectionNote->setWordWrap(true);
    detectionLayout->addRow(detectionNote);
    settingsLayout->addWidget(detectionBox);

    // Capture: the still picture options, between Aspect ratio and Resize
    auto *captureBox = new QGroupBox(tr("Each photo"), settingsWidget);
    auto *captureLayout = new QFormLayout(captureBox);
    captureCombo = new QComboBox(captureBox);
    captureCombo->addItem(tr("Average of its frames"), int(FrameSnapshot::StillMode::Average));
    captureCombo->addItem(tr("Cleanest frame"), int(FrameSnapshot::StillMode::Cleanest));
    captureCombo->setToolTip(tr("Frames with dropouts or tears are left out either way. The average removes "
                                "most tape noise."));
    captureCombo->setCurrentIndex(std::max(0, captureCombo->findData(int(settings.snapshot.stillMode))));
    radiusSpin = new QSpinBox(captureBox);
    radiusSpin->setRange(1, 600);
    radiusSpin->setPrefix(tr("±"));
    radiusSpin->setSuffix(tr(" frames"));
    radiusSpin->setValue(settings.snapshot.searchRadius);
    radiusSpin->setToolTip(tr("Frames used either side of the middle of each photo's hold."));
    captureLayout->addRow(tr("Save:"), captureCombo);
    captureLayout->addRow(tr("Use up to:"), radiusSpin);

    controls = new FrameSnapshotControls(source.videoParameters, source.frameSize, preview, captureBox, settingsWidget);
    controls->setOptions(settings.snapshot);
    settingsLayout->addWidget(controls);

    // Output folder
    auto *folderBox = new QGroupBox(tr("Save to folder"), settingsWidget);
    auto *folderLayout = new QHBoxLayout(folderBox);
    folderEdit = new QLineEdit(settings.outputDirectory.isEmpty() ? source.defaultDirectory : settings.outputDirectory,
                               folderBox);
    auto *browseButton = new QPushButton(tr("Browse..."), folderBox);
    browseButton->setAutoDefault(false);
    folderLayout->addWidget(folderEdit, 1);
    folderLayout->addWidget(browseButton);
    settingsLayout->addWidget(folderBox);
    settingsLayout->addStretch(1);

    // Scan, progress and the dialog buttons stay in view below the settings
    scanButton = new QPushButton(tr("Scan"), this);
    progressBar = new QProgressBar(this);
    progressBar->setVisible(false);
    statusLabel = new QLabel(tr("Scan to list the photos held on this stretch of tape."), this);
    statusLabel->setWordWrap(true);
    rightLayout->addWidget(scanButton);
    rightLayout->addWidget(progressBar);
    rightLayout->addWidget(statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    saveButton = buttons->addButton(tr("Save stills"), QDialogButtonBox::ActionRole);
    saveButton->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    rightLayout->addWidget(buttons);
    scanButton->setDefault(true);

    progressTimer = new QTimer(this);
    progressTimer->setInterval(100);
    connect(progressTimer, &QTimer::timeout, this, [this]() {
        if (scanProgress) progressBar->setValue(scanProgress->load());
    });

    connect(fromSpin, &QSpinBox::valueChanged, this, [this]() { refreshRange(); });
    connect(toSpin, &QSpinBox::valueChanged, this, [this]() { refreshRange(); });
    connect(wholeButton, &QPushButton::clicked, this, [this]() {
        fromSpin->setValue(1);
        toSpin->setValue(this->source.frameCount);
    });
    connect(inOutButton, &QPushButton::clicked, this, [this]() {
        fromSpin->setValue(this->source.inPoint);
        toSpin->setValue(this->source.outPoint);
    });
    connect(browseButton, &QPushButton::clicked, this, &SlideshowDialog::browse);
    connect(scanButton, &QPushButton::clicked, this, &SlideshowDialog::startScan);
    connect(&scanWatcher, &QFutureWatcher<ScanOutcome>::finished, this, &SlideshowDialog::scanFinished);
    connect(saveButton, &QPushButton::clicked, this, &SlideshowDialog::save);
    connect(allButton, &QPushButton::clicked, this, [this]() {
        for (qint32 i = 0; i < holdList->topLevelItemCount(); i++) holdList->topLevelItem(i)->setCheckState(StillColumn, Qt::Checked);
    });
    connect(noneButton, &QPushButton::clicked, this, [this]() {
        for (qint32 i = 0; i < holdList->topLevelItemCount(); i++) holdList->topLevelItem(i)->setCheckState(StillColumn, Qt::Unchecked);
    });
    connect(holdList, &QTreeWidget::itemChanged, this, [this]() { refreshSaveButton(); });
    connect(holdList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { showHold(item); });
    connect(holdList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const qint32 index = item->data(StillColumn, Qt::UserRole).toInt();
        if (index >= 0 && index < holds.size()) emit jumpRequested(holds[index].middle());
    });

    refreshRange();
    resize(1280, 820);
}

SlideshowDialog::~SlideshowDialog()
{
    // The worker holds its own references to these; it stops at its next frame
    if (scanCancel) scanCancel->store(true);
}

FrameSnapshot::Options SlideshowDialog::currentOptions() const
{
    FrameSnapshot::Options options;
    controls->applyTo(options);
    options.stillMode = static_cast<FrameSnapshot::StillMode>(captureCombo->currentData().toInt());
    options.searchRadius = radiusSpin->value();
    return options;
}

SlideshowExtractOptions SlideshowDialog::currentSettings() const
{
    SlideshowExtractOptions settings;
    settings.snapshot = currentOptions();
    settings.minHoldSeconds = minHoldSpin->value();
    settings.outputDirectory = folderEdit->text().trimmed();
    return settings;
}

void SlideshowDialog::setStatus(const QString &text)
{
    statusLabel->setText(text);
}

void SlideshowDialog::refreshRange()
{
    const VideoSystem system = source.videoParameters.system;
    const qint32 first = fromSpin->value();
    const qint32 last = std::max(first, toSpin->value());
    rangeLabel->setText(tr("%1 to %2 (%3 frames)")
                            .arg(SlideshowExtract::frameTimecode(first, system),
                                 SlideshowExtract::frameTimecode(last, system))
                            .arg(last - first + 1));
    scanButton->setEnabled(scanWatcher.isRunning() || toSpin->value() >= fromSpin->value());
}

void SlideshowDialog::startScan()
{
    if (scanWatcher.isRunning()) {
        scanCancel->store(true);
        scanButton->setEnabled(false);
        return;
    }

    const qint32 first = fromSpin->value();
    const qint32 last = toSpin->value();
    const FrameSnapshot::Options options = currentOptions();
    scannedInput = buildInput(first, last);
    scannedInput.options = options;
    scannedInput.scan.cropRect = FrameSnapshot::outputRect(options, source.videoParameters, source.frameSize);
    const qint32 minHoldFrames = std::max(
        1, qRound(minHoldSpin->value() * SlideshowExtract::frameRate(source.videoParameters.system)));
    emit settingsChanged(currentSettings());

    holds.clear();
    previews.clear();
    holdList->clear();
    refreshSaveButton();

    scanCancel = std::make_shared<std::atomic<bool>>(false);
    scanProgress = std::make_shared<std::atomic<qint32>>(0);
    const SlideshowExtract::ScanInput input = scannedInput.scan;
    const auto cancel = scanCancel;
    const auto progress = scanProgress;
    scanWatcher.setFuture(QtConcurrent::run([input, minHoldFrames, cancel, progress]() {
        ScanOutcome outcome;
        outcome.scan = SlideshowExtract::scan(input, cancel.get(), progress.get());
        if (!outcome.scan.errorMessage.isEmpty() || outcome.scan.cancelled) return outcome;
        outcome.holds = SlideshowExtract::findHolds(outcome.scan.samples, input.firstFrame, minHoldFrames);
        QVector<qint32> middles;
        for (const SlideshowExtract::Hold &hold : outcome.holds) middles.append(hold.middle());
        outcome.previews = SlideshowExtract::framePreviews(input, middles, PREVIEW_WIDTH, cancel.get());
        outcome.scan.samples.clear(); // not needed past here; can be large
        return outcome;
    }));

    progressBar->setRange(0, last - first + 1);
    progressBar->setValue(0);
    progressBar->setVisible(true);
    progressTimer->start();
    scanButton->setText(tr("Stop"));
    statusLabel->setText(tr("Scanning frames %1-%2...").arg(first).arg(last));
}

void SlideshowDialog::scanFinished()
{
    progressTimer->stop();
    progressBar->setVisible(false);
    scanButton->setText(tr("Scan"));
    scanButton->setEnabled(true);

    ScanOutcome outcome = scanWatcher.result();
    if (outcome.scan.cancelled) {
        statusLabel->setText(tr("Scan stopped."));
        return;
    }
    if (!outcome.scan.errorMessage.isEmpty()) {
        statusLabel->setText(outcome.scan.errorMessage);
        return;
    }
    holds = outcome.holds;
    previews = outcome.previews;

    const VideoSystem system = source.videoParameters.system;
    const double rate = SlideshowExtract::frameRate(system);
    const double pixelAspect = FrameSnapshot::pixelAspect(FrameSnapshot::AspectMode::Exact, source.videoParameters);
    qint32 photos = 0;
    {
        const QSignalBlocker blocker(holdList);
        for (qint32 i = 0; i < holds.size(); i++) {
            const SlideshowExtract::Hold &hold = holds[i];
            const bool photo = hold.kind == SlideshowExtract::HoldKind::Photo;
            if (photo) photos++;
            auto *item = new QTreeWidgetItem(holdList);
            item->setData(StillColumn, Qt::UserRole, i);
            item->setText(StillColumn, QString::number(i + 1));
            item->setCheckState(StillColumn, photo ? Qt::Checked : Qt::Unchecked);
            item->setText(StartColumn, SlideshowExtract::frameTimecode(hold.first, system));
            item->setText(LengthColumn, tr("%1 s").arg(hold.length() / rate, 0, 'f', 1));
            item->setText(FramesColumn, tr("%1-%2").arg(hold.first).arg(hold.last));
            item->setText(KindColumn, photo ? tr("Photo")
                                      : hold.kind == SlideshowExtract::HoldKind::Moving ? tr("Pan or zoom")
                                                                                        : tr("Unsteady"));
            if (hold.kind == SlideshowExtract::HoldKind::Unsteady) {
                item->setToolTip(KindColumn, tr("The picture keeps breaking up here (tracking or tape damage)."));
            }
            const QImage &image = previews.value(i);
            if (!image.isNull()) {
                const QSize shown(qRound(image.width() * pixelAspect * THUMBNAIL_HEIGHT / image.height()), THUMBNAIL_HEIGHT);
                item->setIcon(StillColumn, QPixmap::fromImage(image.scaled(shown, Qt::IgnoreAspectRatio,
                                                                           Qt::SmoothTransformation)));
            }
        }
    }
    refreshSaveButton();
    const qint32 others = holds.size() - photos;
    QString found = photos == 1 ? tr("Found 1 photo.") : tr("Found %1 photos.").arg(photos);
    if (others > 0) {
        found += QLatin1Char(' ') + (others == 1 ? tr("1 pan, zoom or unsteady stretch is listed unticked.")
                                                 : tr("%1 pans, zooms or unsteady stretches are listed unticked.").arg(others));
    }
    statusLabel->setText(holds.isEmpty() ? tr("No held pictures found. Try a shorter \"Shortest photo\".")
                                         : found + QLatin1Char(' ') + tr("Untick anything that is not a photo, then save."));
    if (!holds.isEmpty()) saveButton->setDefault(true);
}

void SlideshowDialog::refreshSaveButton()
{
    qint32 ticked = 0;
    for (qint32 i = 0; i < holdList->topLevelItemCount(); i++) {
        if (holdList->topLevelItem(i)->checkState(StillColumn) == Qt::Checked) ticked++;
    }
    saveButton->setText(ticked == 1 ? tr("Save 1 still") : tr("Save %1 stills").arg(ticked));
    saveButton->setEnabled(ticked > 0 && !scanWatcher.isRunning());
}

void SlideshowDialog::showHold(QTreeWidgetItem *item)
{
    const qint32 index = item ? item->data(StillColumn, Qt::UserRole).toInt() : -1;
    if (index >= 0 && index < previews.size() && !previews[index].isNull()) {
        preview->setImage(previews[index], source.frameSize);
    } else {
        preview->setImage(source.currentFrameImage, source.frameSize);
    }
}

void SlideshowDialog::browse()
{
    const QString directory = tbc::ui::runDirectoryDialog(this, tr("Save stills to folder"), folderEdit->text());
    if (!directory.isEmpty()) folderEdit->setText(directory);
}

void SlideshowDialog::save()
{
    const QString directory = folderEdit->text().trimmed();
    if (directory.isEmpty()) {
        statusLabel->setText(tr("Choose a folder to save the stills to."));
        return;
    }
    QVector<SlideshowExtract::Hold> ticked;
    for (qint32 i = 0; i < holdList->topLevelItemCount(); i++) {
        QTreeWidgetItem *item = holdList->topLevelItem(i);
        if (item->checkState(StillColumn) == Qt::Checked) ticked.append(holds.value(item->data(StillColumn, Qt::UserRole).toInt()));
    }
    if (ticked.isEmpty()) return;

    // Framing, aspect and capture as they are now; the holds are as scanned
    SlideshowExtract::CaptureInput input = scannedInput;
    input.options = currentOptions();
    input.scan.cropRect = FrameSnapshot::outputRect(input.options, source.videoParameters, source.frameSize);
    emit settingsChanged(currentSettings());
    emit saveRequested(input, ticked, directory);
}
