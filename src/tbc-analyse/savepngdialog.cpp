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

    // Aspect
    auto *aspectBox = new QGroupBox(tr("Aspect ratio"), this);
    auto *aspectLayout = new QVBoxLayout(aspectBox);
    aspectCombo = new QComboBox(aspectBox);
    aspectCombo->addItem(tr("Square pixels from the sample rate"), int(FrameSnapshot::AspectMode::Exact));
    aspectCombo->addItem(tr("Match the tbc-analyse viewer's DAR stretch"), int(FrameSnapshot::AspectMode::Viewer));
    aspectLayout->addWidget(aspectCombo);
    layout->addWidget(aspectBox);

    if (middleGroup) layout->addWidget(middleGroup);

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
    for (QSpinBox *spin : {marginLeftSpin, marginTopSpin, marginRightSpin, marginBottomSpin}) {
        connect(spin, &QSpinBox::valueChanged, this, [this]() { refresh(); });
    }
    for (QComboBox *combo : {aspectCombo, upscaleCombo, methodCombo}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this]() { refresh(); });
    }
}

void FrameSnapshotControls::setOptions(const FrameSnapshot::Options &options)
{
    // Filled without signals; refresh() below brings everything in line once
    const QList<QObject *> controls = {marginLeftSpin, marginTopSpin, marginRightSpin, marginBottomSpin,
                                       aspectCombo, upscaleCombo, methodCombo};
    for (QObject *control : controls) control->blockSignals(true);

    customRect = options.customRect;
    if (QAbstractButton *button = framingGroup->button(int(options.framing))) button->setChecked(true);
    marginLeftSpin->setValue(options.marginLeft);
    marginTopSpin->setValue(options.marginTop);
    marginRightSpin->setValue(options.marginRight);
    marginBottomSpin->setValue(options.marginBottom);
    aspectCombo->setCurrentIndex(qMax(0, aspectCombo->findData(int(options.aspectMode))));
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
    options.aspectMode = static_cast<FrameSnapshot::AspectMode>(aspectCombo->currentData().toInt());
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

    // Still pictures
    auto *searchBox = new QGroupBox(tr("Still picture"), this);
    auto *searchLayout = new QFormLayout(searchBox);
    stillCombo = new QComboBox(searchBox);
    stillCombo->addItem(tr("This frame only"), int(FrameSnapshot::StillMode::Off));
    stillCombo->addItem(tr("Cleanest nearby frame"), int(FrameSnapshot::StillMode::Cleanest));
    stillCombo->addItem(tr("Average of nearby frames"), int(FrameSnapshot::StillMode::Average));
    stillCombo->setToolTip(tr("For a picture held on tape (a slideshow photo): finds the frames around this one "
                              "that show the same picture and drops those with dropouts or tears. \"Cleanest\" "
                              "saves the most typical of them; \"Average\" saves their mean, which removes most "
                              "tape noise. The viewer moves to the most typical frame."));
    radiusSpin = new QSpinBox(searchBox);
    radiusSpin->setRange(1, 600);
    radiusSpin->setPrefix(tr("±"));
    radiusSpin->setSuffix(tr(" frames"));
    searchLayout->addRow(tr("Save:"), stillCombo);
    searchLayout->addRow(tr("Search up to:"), radiusSpin);

    controls = new FrameSnapshotControls(videoParameters, frameImage.size(), preview, searchBox, this);
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
    connect(stillCombo, &QComboBox::currentIndexChanged, this, [this]() {
        radiusSpin->setEnabled(stillCombo->currentData().toInt() != int(FrameSnapshot::StillMode::Off));
    });

    resize(1100, 620);
}

FrameSnapshot::Options SavePngDialog::selectedOptions() const
{
    FrameSnapshot::Options options;
    controls->applyTo(options);
    options.stillMode = static_cast<FrameSnapshot::StillMode>(stillCombo->currentData().toInt());
    options.searchRadius = radiusSpin->value();
    return options;
}

void SavePngDialog::setControls(const FrameSnapshot::Options &options)
{
    controls->setOptions(options);
    {
        const QSignalBlocker blocker(stillCombo);
        stillCombo->setCurrentIndex(qMax(0, stillCombo->findData(int(options.stillMode))));
    }
    radiusSpin->setValue(options.searchRadius);
    radiusSpin->setEnabled(options.stillMode != FrameSnapshot::StillMode::Off);
}
