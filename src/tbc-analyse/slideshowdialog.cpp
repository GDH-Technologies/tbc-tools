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

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "savepngdialog.h"
#include "timelinemarkerslider.h"
#include "tbc/uistyle.h"
#include "theme_color_tokens.h"

namespace {
// Review list columns
enum Column { StillColumn, StartColumn, LengthColumn, FramesColumn, KindColumn };
constexpr qint32 PREVIEW_WIDTH = 320;   // review list thumbnails
constexpr qint32 SCRUB_WIDTH = 760;     // the scrubber's greyscale preview
constexpr qint32 THUMBNAIL_HEIGHT = 72;
constexpr qint32 LUMA_DELAY_MS = 30;    // after the scrubber moves
constexpr qint32 COLOUR_DELAY_MS = 150; // after it stops
} // namespace

SlideshowDialog::SlideshowDialog(const SlideshowExtractOptions &settings, const Source &source,
                                 InputBuilder buildInput, QWidget *parent)
    : QDialog(parent), source(source), buildInput(std::move(buildInput))
{
    setWindowTitle(tr("Extract slideshow stills"));

    auto *mainLayout = new QHBoxLayout(this);

    // Left and right columns, and on the left the picture over the review
    // list, all resizable; the picture gets most of the room to start with
    columnSplitter = new QSplitter(Qt::Horizontal, this);
    columnSplitter->setChildrenCollapsible(false);
    mainLayout->addWidget(columnSplitter);
    rowSplitter = new QSplitter(Qt::Vertical, columnSplitter);
    rowSplitter->setChildrenCollapsible(false);
    columnSplitter->addWidget(rowSplitter);

    // The picture, with a scrubber over the whole tape that also sets the range
    auto *pictureWidget = new QWidget(rowSplitter);
    auto *pictureLayout = new QVBoxLayout(pictureWidget);
    pictureLayout->setContentsMargins(0, 0, 0, 0);
    preview = new CropPreview(pictureWidget);
    preview->setMinimumSize(320, 200);
    preview->setImage(source.currentFrameImage, source.frameSize);
    preview->setToolTip(tr("Drag a rectangle to frame the picture yourself."));
    pictureLayout->addWidget(preview, 1);

    scrubSlider = new TimelineMarkerSlider(pictureWidget);
    scrubSlider->setOrientation(Qt::Horizontal);
    scrubSlider->setRange(1, std::max(1, source.frameCount));
    scrubSlider->setValue(std::clamp(source.currentFrame, 1, std::max(1, source.frameCount)));
    scrubSlider->setToolTip(tr("Scrub the tape. The green and red lines are the start and end of the frames to "
                               "scan; after a scan, the photos found are tinted."));
    pictureLayout->addWidget(scrubSlider);
    auto *scrubRow = new QHBoxLayout();
    scrubLabel = new QLabel(pictureWidget);
    scrubLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *setStartButton = new QPushButton(tr("Set start"), pictureWidget);
    setStartButton->setShortcut(QKeySequence(Qt::Key_BracketLeft));
    setStartButton->setToolTip(tr("Scan from this frame ([)"));
    auto *setEndButton = new QPushButton(tr("Set end"), pictureWidget);
    setEndButton->setShortcut(QKeySequence(Qt::Key_BracketRight));
    setEndButton->setToolTip(tr("Scan to this frame (])"));
    scrubRow->addWidget(scrubLabel, 1);
    for (QPushButton *button : {setStartButton, setEndButton}) {
        button->setAutoDefault(false);
        scrubRow->addWidget(button);
    }
    pictureLayout->addLayout(scrubRow);
    rowSplitter->addWidget(pictureWidget);

    auto *listWidget = new QWidget(rowSplitter);
    auto *reviewLayout = new QVBoxLayout(listWidget);
    reviewLayout->setContentsMargins(0, 0, 0, 0);
    holdList = new QTreeWidget(listWidget);
    holdList->setHeaderLabels({tr("Still"), tr("Start"), tr("Length"), tr("Frames"), tr("Kind")});
    holdList->setRootIsDecorated(false);
    holdList->setUniformRowHeights(true);
    holdList->setIconSize(QSize(THUMBNAIL_HEIGHT * 2, THUMBNAIL_HEIGHT));
    holdList->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    holdList->setToolTip(tr("Untick anything that is not a photo. Double-click to show the frame in the viewer."));
    reviewLayout->addWidget(holdList, 1);

    auto *listButtons = new QHBoxLayout();
    auto *allButton = new QPushButton(tr("Tick all"), listWidget);
    auto *noneButton = new QPushButton(tr("Untick all"), listWidget);
    for (QPushButton *button : {allButton, noneButton}) {
        button->setAutoDefault(false);
        listButtons->addWidget(button);
    }
    listButtons->addStretch(1);
    reviewLayout->addLayout(listButtons);
    rowSplitter->addWidget(listWidget);
    rowSplitter->setStretchFactor(0, 3);
    rowSplitter->setStretchFactor(1, 2);

    // Right: the settings, scrolling on a small screen
    auto *rightWidget = new QWidget(columnSplitter);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto *settingsWidget = new QWidget(rightWidget);
    auto *settingsLayout = new QVBoxLayout(settingsWidget);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    auto *scrollArea = new QScrollArea(rightWidget);
    scrollArea->setWidget(settingsWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rightLayout->addWidget(scrollArea, 1);
    columnSplitter->addWidget(rightWidget);
    columnSplitter->setStretchFactor(0, 1);
    columnSplitter->setStretchFactor(1, 0);

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

    // Extraction mode, between Framing and Aspect ratio
    extractionControls = new StillModeControls(
        tr("Extraction mode"),
        {{FrameSnapshot::StillMode::Average, tr("Average"),
          tr("Each photo is the mean of its frames, after leaving out those with dropouts or tears and "
             "realigning any that sit off. Removes most tape noise.")},
         {FrameSnapshot::StillMode::Cleanest, tr("Cleanest frame"),
          tr("Each photo is the one real frame closest to the photo's per-pixel median: no averaging, so "
             "grain stays, but nothing is blended.")}},
        settingsWidget);
    extractionControls->setMode(settings.snapshot.stillMode);
    extractionControls->setWindow(settings.snapshot.searchRadius);

    controls = new FrameSnapshotControls(source.videoParameters, source.frameSize, preview, extractionControls,
                                         settingsWidget);
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

    // The scrubber reads the whole tape
    tapeInput = this->buildInput(1, std::max(1, source.frameCount)).scan;
    lumaTimer = new QTimer(this);
    lumaTimer->setSingleShot(true);
    lumaTimer->setInterval(LUMA_DELAY_MS);
    colourTimer = new QTimer(this);
    colourTimer->setSingleShot(true);
    colourTimer->setInterval(COLOUR_DELAY_MS);
    connect(lumaTimer, &QTimer::timeout, this, &SlideshowDialog::renderLuma);
    connect(colourTimer, &QTimer::timeout, this, [this]() { emit colourFrameRequested(scrubSlider->value()); });
    connect(&lumaWatcher, &QFutureWatcher<QImage>::finished, this, [this]() {
        const QImage image = lumaWatcher.result();
        if (lumaFrame == scrubSlider->value() && !image.isNull()) preview->setImage(image, this->source.frameSize);
        // The slider moved on while this one rendered
        if (lumaFrame != scrubSlider->value()) renderLuma();
    });
    connect(scrubSlider, &QSlider::valueChanged, this, &SlideshowDialog::scrubTo);
    connect(setStartButton, &QPushButton::clicked, this, [this]() {
        fromSpin->setValue(scrubSlider->value());
        if (toSpin->value() < fromSpin->value()) toSpin->setValue(this->source.frameCount);
    });
    connect(setEndButton, &QPushButton::clicked, this, [this]() {
        toSpin->setValue(scrubSlider->value());
        if (fromSpin->value() > toSpin->value()) fromSpin->setValue(1);
    });

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
        if (index >= 0 && index < holds.size()) emit jumpRequested(holds[index].anchor);
    });

    // The settings column scrolls with the wheel: its spin boxes, combo boxes
    // and sliders take it only once clicked into, like every field in the
    // program (tbc/wheelguard.h, installed by main)

    refreshRange();
    scrubTo(scrubSlider->value());
    resize(1280, 860);
    columnSplitter->setSizes({900, 380});
    rowSplitter->setSizes({520, 340});
    if (!settings.dialogGeometry.isEmpty()) restoreGeometry(settings.dialogGeometry);
    if (!settings.dialogColumns.isEmpty()) columnSplitter->restoreState(settings.dialogColumns);
    if (!settings.dialogRows.isEmpty()) rowSplitter->restoreState(settings.dialogRows);
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
    options.stillMode = extractionControls->mode();
    options.searchRadius = extractionControls->window();
    return options;
}

SlideshowExtractOptions SlideshowDialog::currentSettings() const
{
    SlideshowExtractOptions settings;
    settings.snapshot = currentOptions();
    settings.minHoldSeconds = minHoldSpin->value();
    settings.outputDirectory = folderEdit->text().trimmed();
    settings.dialogGeometry = saveGeometry();
    settings.dialogColumns = columnSplitter->saveState();
    settings.dialogRows = rowSplitter->saveState();
    return settings;
}

void SlideshowDialog::setStatus(const QString &text)
{
    statusLabel->setText(text);
}

void SlideshowDialog::setPreviewFrame(qint32 frame, const QImage &image)
{
    if (frame == scrubSlider->value() && !image.isNull()) preview->setImage(image, source.frameSize);
}

void SlideshowDialog::closeEvent(QCloseEvent *event)
{
    // Keeps the size and splitter positions (and the settings) for next time
    emit settingsChanged(currentSettings());
    QDialog::closeEvent(event);
}

void SlideshowDialog::paintEvent(QPaintEvent *event)
{
    QDialog::paintEvent(event);
    // An outline, so the window's edge shows against the main window in a
    // dark theme: a step from the window colour toward its text
    QPainter painter(this);
    painter.setPen(QPen(theme_tokens::neutralLine(palette(), 0.35), 2));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));
}

void SlideshowDialog::changeEvent(QEvent *event)
{
    // The hold tints and the outline come from the palette; follow a theme switch
    if (event->type() == QEvent::PaletteChange) {
        refreshHoldSpans();
        update();
    }
    QDialog::changeEvent(event);
}

void SlideshowDialog::scrubTo(qint32 frame)
{
    scrubLabel->setText(tr("Frame %1  %2").arg(frame).arg(SlideshowExtract::frameTimecode(frame, source.videoParameters.system)));
    lumaTimer->start();
    colourTimer->start();
}

void SlideshowDialog::renderLuma()
{
    if (lumaWatcher.isRunning()) return; // its finish picks up the latest frame
    lumaFrame = scrubSlider->value();
    const SlideshowExtract::ScanInput input = tapeInput;
    const qint32 frame = lumaFrame;
    lumaWatcher.setFuture(QtConcurrent::run([input, frame]() {
        return SlideshowExtract::framePreviews(input, {frame}, SCRUB_WIDTH).value(0);
    }));
}

void SlideshowDialog::refreshHoldSpans()
{
    QColor photoColour = palette().color(QPalette::Highlight);
    photoColour.setAlpha(140);
    QColor otherColour = palette().color(QPalette::PlaceholderText);
    otherColour.setAlpha(160);
    QVector<TimelineSegmentSpan> spans;
    for (const SlideshowExtract::Hold &hold : holds) {
        TimelineSegmentSpan span;
        span.startPosition = hold.first;
        span.endPosition = hold.last;
        span.color = hold.kind == SlideshowExtract::HoldKind::Photo ? photoColour : otherColour;
        spans.append(span);
    }
    scrubSlider->setSegmentMarkers({}, {}, spans);
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
    scrubSlider->setMarkerFrames(first, last, {});
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
    refreshHoldSpans();
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
        QVector<qint32> anchors;
        for (const SlideshowExtract::Hold &hold : outcome.holds) anchors.append(hold.anchor);
        outcome.previews = SlideshowExtract::framePreviews(input, anchors, PREVIEW_WIDTH, cancel.get());
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
    refreshHoldSpans();
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

// A selected row puts the scrubber on the photo's most typical frame
void SlideshowDialog::showHold(QTreeWidgetItem *item)
{
    const qint32 index = item ? item->data(StillColumn, Qt::UserRole).toInt() : -1;
    if (index >= 0 && index < holds.size()) scrubSlider->setValue(holds[index].anchor);
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
