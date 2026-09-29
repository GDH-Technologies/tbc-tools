/******************************************************************************
 * savepngdialog.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "savepngdialog.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

CropPreview::CropPreview(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(480, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
}

void CropPreview::setImage(const QImage &newImage, const QSize &newFrameSize)
{
    image = newImage;
    frameSize = newFrameSize.isEmpty() ? newImage.size() : newFrameSize;
    update();
}

void CropPreview::setOverlay(const QRect &rect, double aspect)
{
    overlayRect = rect;
    pixelAspect = aspect;
    update();
}

void CropPreview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    if (image.isNull() || frameSize.isEmpty()) return;

    const QRectF target = imageTarget();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, image);

    const QRect shown = dragging ? QRect(dragStart, dragEnd).normalized() : overlayRect;
    if (shown.isEmpty()) return;
    const QRectF overlay = toWidget(shown);

    // Dim everything outside the output rectangle
    QPainterPath outside;
    outside.addRect(target);
    QPainterPath inside;
    inside.addRect(overlay);
    painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 150));
    painter.setPen(QPen(QColor(255, 64, 64), 2));
    painter.drawRect(overlay);
}

void CropPreview::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || image.isNull()) return;
    dragging = true;
    dragStart = dragEnd = toFrame(event->position());
    update();
}

void CropPreview::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging) return;
    dragEnd = toFrame(event->position());
    update();
}

void CropPreview::mouseReleaseEvent(QMouseEvent *event)
{
    if (!dragging || event->button() != Qt::LeftButton) return;
    dragging = false;
    dragEnd = toFrame(event->position());
    const QRect drawn = QRect(dragStart, dragEnd).normalized();
    // A click without a drag is not a rectangle
    if (drawn.width() >= 8 && drawn.height() >= 8 && onRectDrawn) onRectDrawn(drawn);
    update();
}

QRectF CropPreview::imageTarget() const
{
    const QSizeF displaySize(frameSize.width() * pixelAspect, frameSize.height());
    const QSizeF fitted = displaySize.scaled(QSizeF(size()), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0), fitted);
}

QRectF CropPreview::toWidget(const QRect &frameRect) const
{
    const QRectF target = imageTarget();
    const double sx = target.width() / frameSize.width();
    const double sy = target.height() / frameSize.height();
    return QRectF(target.left() + frameRect.left() * sx, target.top() + frameRect.top() * sy,
                  frameRect.width() * sx, frameRect.height() * sy);
}

QPoint CropPreview::toFrame(const QPointF &point) const
{
    const QRectF target = imageTarget();
    const int x = qBound(0, int((point.x() - target.left()) * frameSize.width() / target.width()), frameSize.width() - 1);
    const int y = qBound(0, int((point.y() - target.top()) * frameSize.height() / target.height()), frameSize.height() - 1);
    return QPoint(x, y);
}

FrameSnapshotControls::FrameSnapshotControls(const TbcMetaData::VideoParameters &videoParameters,
                                             const QSize &frameSize, CropPreview *preview,
                                             QWidget *middleGroup, QWidget *parent)
    : QWidget(parent), videoParameters(videoParameters), frameSize(frameSize), preview(preview)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // Framing
    auto *framingBox = new QGroupBox(tr("Framing"), this);
    auto *framingLayout = new QVBoxLayout(framingBox);
    auto *fullRadio = new QRadioButton(tr("Full frame (blanking and sync included)"), framingBox);
    activeRadio = new QRadioButton(tr("Active picture area"), framingBox);
    customRadio = new QRadioButton(tr("Custom rectangle"), framingBox);
    customRadio->setToolTip(tr("Drag on the preview to draw the rectangle."));
    framingGroup = new QButtonGroup(this);
    framingGroup->addButton(fullRadio, int(FrameSnapshot::Framing::Full));
    framingGroup->addButton(activeRadio, int(FrameSnapshot::Framing::Active));
    framingGroup->addButton(customRadio, int(FrameSnapshot::Framing::Custom));
    framingLayout->addWidget(fullRadio);
    framingLayout->addWidget(activeRadio);
    framingLayout->addWidget(customRadio);
    customLabel = new QLabel(framingBox);
    framingLayout->addWidget(customLabel);

    auto *marginsLayout = new QGridLayout();
    auto makeMarginSpin = [framingBox]() {
        auto *spin = new QSpinBox(framingBox);
        spin->setRange(0, 400);
        return spin;
    };
    marginLeftSpin = makeMarginSpin();
    marginTopSpin = makeMarginSpin();
    marginRightSpin = makeMarginSpin();
    marginBottomSpin = makeMarginSpin();
    marginsLayout->addWidget(new QLabel(tr("Trim left"), framingBox), 0, 0);
    marginsLayout->addWidget(marginLeftSpin, 0, 1);
    marginsLayout->addWidget(new QLabel(tr("right"), framingBox), 0, 2);
    marginsLayout->addWidget(marginRightSpin, 0, 3);
    marginsLayout->addWidget(new QLabel(tr("Trim top"), framingBox), 1, 0);
    marginsLayout->addWidget(marginTopSpin, 1, 1);
    marginsLayout->addWidget(new QLabel(tr("bottom"), framingBox), 1, 2);
    marginsLayout->addWidget(marginBottomSpin, 1, 3);
    framingLayout->addLayout(marginsLayout);
    auto *marginsNote = new QLabel(tr("Trims are in samples (left/right) and frame lines (top/bottom)."), framingBox);
    marginsNote->setWordWrap(true);
    framingLayout->addWidget(marginsNote);
    layout->addWidget(framingBox);

    if (middleGroup) layout->addWidget(middleGroup);

    // Aspect
    auto *aspectBox = new QGroupBox(tr("Aspect ratio"), this);
    auto *aspectLayout = new QHBoxLayout(aspectBox);
    auto *exactRadio = new QRadioButton(tr("Exact"), aspectBox);
    exactRadio->setToolTip(tr("Square pixels from the decoded sample rate (ITU-R BT.601): the picture's true "
                              "shape. NTSC 4fsc samples are narrowed by 6/7."));
    auto *viewerRadio = new QRadioButton(tr("Viewer DAR"), aspectBox);
    viewerRadio->setToolTip(tr("The fixed width stretch tbc-analyse's viewer applies in DAR mode, so the PNG "
                               "matches what the viewer shows."));
    aspectGroup = new QButtonGroup(this);
    aspectGroup->addButton(exactRadio, int(FrameSnapshot::AspectMode::Exact));
    aspectGroup->addButton(viewerRadio, int(FrameSnapshot::AspectMode::Viewer));
    aspectLayout->addWidget(exactRadio);
    aspectLayout->addWidget(viewerRadio);
    aspectLayout->addStretch(1);
    layout->addWidget(aspectBox);

    // Resize
    auto *resizeBox = new QGroupBox(tr("Resize"), this);
    auto *resizeLayout = new QFormLayout(resizeBox);
    upscaleCombo = new QComboBox(resizeBox);
    upscaleCombo->addItem(tr("Off"), 1);
    upscaleCombo->addItem(tr("2×"), 2);
    upscaleCombo->addItem(tr("3×"), 3);
    upscaleCombo->addItem(tr("4×"), 4);
    methodCombo = new QComboBox(resizeBox);
    for (const FrameSnapshot::UpscaleMethod &method : FrameSnapshot::upscaleMethods()) {
        methodCombo->addItem(method.label, method.name);
    }
    methodCombo->setToolTip(tr("Used for the upscale and for the aspect correction. The learned models "
                               "only upscale; their aspect correction uses Lanczos-4."));
    resizeLayout->addRow(tr("Upscale:"), upscaleCombo);
    resizeLayout->addRow(tr("Method:"), methodCombo);
    layout->addWidget(resizeBox);

    outputLabel = new QLabel(this);
    layout->addWidget(outputLabel);

    preview->onRectDrawn = [this](const QRect &rect) {
        // A drawn rectangle is the framing; start its trims from zero
        customRect = rect;
        for (QSpinBox *spin : {marginLeftSpin, marginTopSpin, marginRightSpin, marginBottomSpin}) {
            const QSignalBlocker blocker(spin);
            spin->setValue(0);
        }
        customRadio->setChecked(true);
        refresh();
    };

    connect(framingGroup, &QButtonGroup::idClicked, this, [this]() { refresh(); });
    connect(aspectGroup, &QButtonGroup::idClicked, this, [this]() { refresh(); });
    for (QSpinBox *spin : {marginLeftSpin, marginTopSpin, marginRightSpin, marginBottomSpin}) {
        connect(spin, &QSpinBox::valueChanged, this, [this]() { refresh(); });
    }
    for (QComboBox *combo : {upscaleCombo, methodCombo}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this]() { refresh(); });
    }
}

void FrameSnapshotControls::setOptions(const FrameSnapshot::Options &options)
{
    // Filled without signals; refresh() below brings everything in line once
    const QList<QObject *> controls = {marginLeftSpin, marginTopSpin, marginRightSpin, marginBottomSpin,
                                       upscaleCombo, methodCombo};
    for (QObject *control : controls) control->blockSignals(true);

    customRect = options.customRect;
    if (QAbstractButton *button = framingGroup->button(int(options.framing))) button->setChecked(true);
    marginLeftSpin->setValue(options.marginLeft);
    marginTopSpin->setValue(options.marginTop);
    marginRightSpin->setValue(options.marginRight);
    marginBottomSpin->setValue(options.marginBottom);
    if (QAbstractButton *button = aspectGroup->button(int(options.aspectMode))) button->setChecked(true);
    upscaleCombo->setCurrentIndex(qMax(0, upscaleCombo->findData(options.upscaleFactor)));
    methodCombo->setCurrentIndex(qMax(0, methodCombo->findData(options.upscaleMethod)));

    for (QObject *control : controls) control->blockSignals(false);
    refresh();
}

void FrameSnapshotControls::applyTo(FrameSnapshot::Options &options) const
{
    options.framing = static_cast<FrameSnapshot::Framing>(framingGroup->checkedId());
    options.marginLeft = marginLeftSpin->value();
    options.marginTop = marginTopSpin->value();
    options.marginRight = marginRightSpin->value();
    options.marginBottom = marginBottomSpin->value();
    options.customRect = customRect;
    options.aspectMode = static_cast<FrameSnapshot::AspectMode>(aspectGroup->checkedId());
    options.upscaleFactor = upscaleCombo->currentData().toInt();
    options.upscaleMethod = methodCombo->currentData().toString();
}

void FrameSnapshotControls::refresh()
{
    customRadio->setEnabled(!customRect.isEmpty());
    if (customRadio->isChecked() && customRect.isEmpty()) activeRadio->setChecked(true);
    customLabel->setText(customRect.isEmpty()
                             ? tr("Drag on the preview to draw a custom rectangle.")
                             : tr("Custom: %1×%2 at %3, %4").arg(customRect.width()).arg(customRect.height())
                                   .arg(customRect.x()).arg(customRect.y()));

    FrameSnapshot::Options options;
    applyTo(options);
    const QRect rect = FrameSnapshot::outputRect(options, videoParameters, frameSize);
    preview->setOverlay(rect, FrameSnapshot::pixelAspect(options.aspectMode, videoParameters));

    const QSize size = FrameSnapshot::outputSize(options, videoParameters, rect.size());
    outputLabel->setText(tr("Output: %1 × %2 pixels").arg(size.width()).arg(size.height()));
    emit changed();
}

StillModeControls::StillModeControls(const QString &title, const QVector<Mode> &modes, QWidget *parent)
    : QGroupBox(title, parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *modeLayout = new QHBoxLayout();
    modeGroup = new QButtonGroup(this);
    for (const Mode &mode : modes) {
        auto *radio = new QRadioButton(mode.label, this);
        radio->setToolTip(mode.toolTip);
        modeGroup->addButton(radio, int(mode.mode));
        modeLayout->addWidget(radio);
    }
    modeLayout->addStretch(1);
    layout->addLayout(modeLayout);

    auto *windowLayout = new QFormLayout();
    windowLabel = new QLabel(this);
    windowSpin = new QSpinBox(this);
    windowSpin->setRange(1, 600);
    windowSpin->setPrefix(tr("\u00b1"));
    windowSpin->setSuffix(tr(" frames"));
    windowLayout->addRow(windowLabel, windowSpin);
    layout->addLayout(windowLayout);
    windowSlider = new QSlider(Qt::Horizontal, this);
    windowSlider->setRange(windowSpin->minimum(), windowSpin->maximum());
    layout->addWidget(windowSlider);

    connect(modeGroup, &QButtonGroup::idClicked, this, [this]() {
        refresh();
        emit changed();
    });
    connect(windowSpin, &QSpinBox::valueChanged, this, [this](int value) {
        const QSignalBlocker blocker(windowSlider);
        windowSlider->setValue(value);
        emit changed();
    });
    connect(windowSlider, &QSlider::valueChanged, windowSpin, &QSpinBox::setValue);
    if (!modes.isEmpty()) setMode(modes.first().mode);
}

FrameSnapshot::StillMode StillModeControls::mode() const
{
    return static_cast<FrameSnapshot::StillMode>(modeGroup->checkedId());
}

qint32 StillModeControls::window() const
{
    return windowSpin->value();
}

void StillModeControls::setMode(FrameSnapshot::StillMode mode)
{
    QAbstractButton *button = modeGroup->button(int(mode));
    if (!button) button = modeGroup->buttons().value(0);
    if (button) button->setChecked(true);
    refresh();
}

void StillModeControls::setWindow(qint32 frames)
{
    windowSpin->setValue(frames);
}

void StillModeControls::refresh()
{
    const FrameSnapshot::StillMode current = mode();
    const bool averaging = current == FrameSnapshot::StillMode::Average;
    windowLabel->setText(averaging ? tr("Averaging window:") : tr("Search window:"));
    const QString toolTip =
        averaging ? tr("Frames either side of the picture's most typical frame that are checked; the ones "
                       "without dropouts or tears are averaged. A larger window removes more noise, up to "
                       "the length of the hold.")
                  : tr("Frames either side of the picture's most typical frame that are searched for the "
                       "cleanest one. Only frames showing the same picture count.");
    for (QWidget *widget : {static_cast<QWidget *>(windowLabel), static_cast<QWidget *>(windowSpin),
                            static_cast<QWidget *>(windowSlider)}) {
        widget->setToolTip(toolTip);
        widget->setEnabled(current != FrameSnapshot::StillMode::Off);
    }
}

SavePngDialog::SavePngDialog(const FrameSnapshot::Options &current, const QImage &frameImage,
                             const TbcMetaData::VideoParameters &videoParameters, bool frameView,
                             QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Save frame as PNG"));

    auto *mainLayout = new QHBoxLayout(this);

    auto *preview = new CropPreview(this);
    preview->setImage(frameImage, frameImage.size());
    preview->setToolTip(tr("Drag a rectangle to frame the picture yourself."));
    mainLayout->addWidget(preview, 1);

    auto *optionsLayout = new QVBoxLayout();
    mainLayout->addLayout(optionsLayout);

    if (!frameView) {
        auto *viewNote = new QLabel(tr("Framing, aspect and upscaling apply to the Frame view only. "
                                       "This view is saved as shown."), this);
        viewNote->setWordWrap(true);
        optionsLayout->addWidget(viewNote);
    }

    // Still pictures (a slideshow photo held on tape)
    stillControls = new StillModeControls(
        tr("Still picture"),
        {{FrameSnapshot::StillMode::Off, tr("This frame"), tr("Save the frame on screen as it is.")},
         {FrameSnapshot::StillMode::Cleanest, tr("Cleanest nearby"),
          tr("Find the frames around this one that show the same picture, drop those with dropouts or tears, "
             "and save the most typical of them. The viewer moves to it.")},
         {FrameSnapshot::StillMode::Average, tr("Average nearby"),
          tr("Find the frames around this one that show the same picture, drop those with dropouts or tears, "
             "realign any that sit off, and save their mean, which removes most tape noise.")}},
        this);

    controls = new FrameSnapshotControls(videoParameters, frameImage.size(), preview, stillControls, this);
    optionsLayout->addWidget(controls);
    optionsLayout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    buttons->addButton(tr("Save..."), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            [this]() { setControls(FrameSnapshot::Options()); });
    optionsLayout->addWidget(buttons);

    setControls(current);

    resize(1100, 620);
}

FrameSnapshot::Options SavePngDialog::selectedOptions() const
{
    FrameSnapshot::Options options;
    controls->applyTo(options);
    options.stillMode = stillControls->mode();
    options.searchRadius = stillControls->window();
    return options;
}

void SavePngDialog::setControls(const FrameSnapshot::Options &options)
{
    controls->setOptions(options);
    stillControls->setMode(options.stillMode);
    stillControls->setWindow(options.searchRadius);
}
