/******************************************************************************
 * segmentsviewerdialog.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/
#include "segmentsviewerdialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtMath>

namespace {

const QStringList kKinds = {QStringLiteral("clip"), QStringLiteral("blank"), QStringLiteral("noise"), QStringLiteral("unknown")};

} // namespace

SegmentsViewerDialog::SegmentsViewerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Segments Viewer"));
    setMinimumSize(900, 520);

    auto *mainLayout = new QVBoxLayout(this);

    infoLabel_ = new QLabel(this);
    infoLabel_->setWordWrap(true);
    mainLayout->addWidget(infoLabel_);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({tr("#"), tr("Start"), tr("End"), tr("Frames"), tr("Duration"),
                                       tr("Kind"), tr("Source"), tr("Enabled"), tr("Title"), tr("Comment")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    for (int c = 0; c < ColTitle; ++c) {
        table_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    table_->horizontalHeader()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColComment, QHeaderView::Stretch);
    mainLayout->addWidget(table_, 1);

    auto *navLayout = new QHBoxLayout();
    goStartButton_ = new QPushButton(tr("Go To Start"), this);
    goEndButton_ = new QPushButton(tr("Go To End"), this);
    setInOutButton_ = new QPushButton(tr("Set In/Out From Segment"), this);
    for (QPushButton *button : {goStartButton_, goEndButton_, setInOutButton_}) {
        button->setAutoDefault(false);
        navLayout->addWidget(button);
    }
    navLayout->addStretch(1);
    mainLayout->addLayout(navLayout);

    auto *editLayout = new QHBoxLayout();
    moveStartButton_ = new QPushButton(tr("Move Start Here"), this);
    moveEndButton_ = new QPushButton(tr("Move End Here"), this);
    splitButton_ = new QPushButton(tr("Split At Current Frame"), this);
    mergeButton_ = new QPushButton(tr("Merge With Next"), this);
    deleteButton_ = new QPushButton(tr("Delete"), this);
    for (QPushButton *button : {moveStartButton_, moveEndButton_, splitButton_, mergeButton_, deleteButton_}) {
        button->setAutoDefault(false);
        editLayout->addWidget(button);
    }
    editLayout->addStretch(1);
    mainLayout->addLayout(editLayout);

    auto *deriveLayout = new QGridLayout();
    deriveLayout->addWidget(new QLabel(tr("Re-derive"), this), 0, 0);
    presetCombo_ = new QComboBox(this);
    presetCombo_->addItems({tr("Low sensitivity"), tr("Normal"), tr("High sensitivity"), tr("Custom")});
    presetCombo_->setCurrentIndex(1);
    deriveLayout->addWidget(presetCombo_, 0, 1);
    deriveLayout->addWidget(new QLabel(tr("Min clip fields"), this), 0, 2);
    minClipFieldsSpin_ = new QSpinBox(this);
    minClipFieldsSpin_->setRange(1, 100000);
    deriveLayout->addWidget(minClipFieldsSpin_, 0, 3);
    deriveLayout->addWidget(new QLabel(tr("Min non-clip run"), this), 0, 4);
    minNonClipRunSpin_ = new QSpinBox(this);
    minNonClipRunSpin_->setRange(1, 100000);
    deriveLayout->addWidget(minNonClipRunSpin_, 0, 5);
    deriveLayout->addWidget(new QLabel(tr("Noise IRE"), this), 0, 6);
    noiseIreSpin_ = new QDoubleSpinBox(this);
    noiseIreSpin_->setRange(0.1, 100.0);
    noiseIreSpin_->setDecimals(1);
    deriveLayout->addWidget(noiseIreSpin_, 0, 7);
    deriveLayout->addWidget(new QLabel(tr("Scene IRE"), this), 0, 8);
    sceneIreSpin_ = new QDoubleSpinBox(this);
    sceneIreSpin_->setRange(0.1, 100.0);
    sceneIreSpin_->setDecimals(1);
    deriveLayout->addWidget(sceneIreSpin_, 0, 9);
    rederiveButton_ = new QPushButton(tr("Re-derive"), this);
    rederiveButton_->setAutoDefault(false);
    rederiveButton_->setToolTip(tr("Derive segments again from the stored decoder events and picture metrics. "
                                   "User-edited segments are kept; derived segments they overlap are dropped."));
    deriveLayout->addWidget(rederiveButton_, 0, 10);
    deriveLayout->setColumnStretch(11, 1);
    mainLayout->addLayout(deriveLayout);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch(1);
    applyButton_ = new QPushButton(tr("Apply"), this);
    applyButton_->setAutoDefault(false);
    auto *closeButton = new QPushButton(tr("Close"), this);
    closeButton->setAutoDefault(false);
    buttonLayout->addWidget(applyButton_);
    buttonLayout->addWidget(closeButton);
    mainLayout->addLayout(buttonLayout);

    connect(table_, &QTableWidget::itemSelectionChanged, this, &SegmentsViewerDialog::updateButtons);
    connect(table_, &QTableWidget::cellChanged, this, &SegmentsViewerDialog::handleItemChanged);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (row < 0 || row >= segments_.size() || column == ColTitle || column == ColComment
            || column == ColEnabled || column == ColKind) {
            return;
        }
        emit goToFieldRequested(segments_.at(row).startField);
    });
    connect(goStartButton_, &QPushButton::clicked, this, [this]() {
        const int row = selectedRow();
        if (row >= 0) {
            emit goToFieldRequested(segments_.at(row).startField);
        }
    });
    connect(goEndButton_, &QPushButton::clicked, this, [this]() {
        const int row = selectedRow();
        if (row >= 0) {
            emit goToFieldRequested(qMax(segments_.at(row).startField, segments_.at(row).endFieldExclusive - 2));
        }
    });
    connect(setInOutButton_, &QPushButton::clicked, this, [this]() {
        const int row = selectedRow();
        if (row < 0) {
            return;
        }
        if (dirty_) {
            emit segmentsUpdated(segments_);
            dirty_ = false;
        }
        emit setInOutRequested(row);
    });
    connect(moveStartButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::moveStartHere);
    connect(moveEndButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::moveEndHere);
    connect(splitButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::splitAtCurrentFrame);
    connect(mergeButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::mergeWithNext);
    connect(deleteButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::deleteSelected);
    connect(presetCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SegmentsViewerDialog::applyPreset);
    connect(rederiveButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::rederive);
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        emit segmentsUpdated(segments_);
        dirty_ = false;
        updateButtons();
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    applyPreset(1);   // after every button exists: it refreshes their enabled state
    updateInfoLabel();
}

void SegmentsViewerDialog::setState(const SegmentsViewerState &state)
{
    state_ = state;
    state_.frameRate = qMax(0.001, state.frameRate);
    state_.frameBaseRate = qMax<qint32>(1, state.frameBaseRate);
    if (!dirty_) {
        QVector<TbcMetaData::Segment> incoming;
        incoming.reserve(state.rows.size());
        for (const SegmentsViewerRow &row : state.rows) {
            incoming.append(row.segment);
        }
        bool same = incoming.size() == segments_.size();
        for (int i = 0; same && i < incoming.size(); ++i) {
            const TbcMetaData::Segment &a = incoming.at(i);
            const TbcMetaData::Segment &b = segments_.at(i);
            same = a.id == b.id && a.startField == b.startField && a.endFieldExclusive == b.endFieldExclusive
                   && a.kind == b.kind && a.source == b.source && a.enabled == b.enabled
                   && a.title == b.title && a.comment == b.comment;
        }
        if (!same || table_->rowCount() != segments_.size()) {
            segments_ = incoming;
            rebuildTable();
        }
    } else {
        // Keep unapplied edits; refresh only the frame-dependent columns
        for (int row = 0; row < segments_.size() && row < table_->rowCount(); ++row) {
            refreshRow(row);
        }
    }
    updateInfoLabel();
    updateButtons();
}

QString SegmentsViewerDialog::frameToTimecode(qint32 frameNumber) const
{
    if (frameNumber <= 0) {
        return QStringLiteral("—");
    }
    const qint64 frameIndex = qMax<qint64>(0, static_cast<qint64>(frameNumber) - 1);
    const double totalSecondsExact = static_cast<double>(frameIndex) / state_.frameRate;
    const qint64 totalSeconds = static_cast<qint64>(qFloor(totalSecondsExact));
    const double fractionalSeconds = totalSecondsExact - static_cast<double>(totalSeconds);
    const int framePart = qBound(0,
                                 static_cast<int>(qFloor((fractionalSeconds * state_.frameBaseRate) + 1e-9)),
                                 state_.frameBaseRate - 1);
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(framePart, 2, 10, QChar('0'));
}

QString SegmentsViewerDialog::fieldsToDuration(qint32 fields) const
{
    const double seconds = static_cast<double>(qMax<qint32>(0, fields)) / (2.0 * state_.frameRate);
    const qint64 whole = static_cast<qint64>(qFloor(seconds));
    const int tenths = static_cast<int>(qFloor((seconds - static_cast<double>(whole)) * 10.0 + 1e-9));
    return QStringLiteral("%1:%2:%3.%4")
        .arg(whole / 3600, 2, 10, QChar('0'))
        .arg((whole % 3600) / 60, 2, 10, QChar('0'))
        .arg(whole % 60, 2, 10, QChar('0'))
        .arg(tenths);
}

void SegmentsViewerDialog::rebuildTable()
{
    applyingState_ = true;
    const QSignalBlocker blocker(table_);
    const int previousRow = table_->currentRow();
    table_->setRowCount(0);
    table_->setRowCount(segments_.size());
    for (int row = 0; row < segments_.size(); ++row) {
        const TbcMetaData::Segment &segment = segments_.at(row);
        for (int c = 0; c < ColCount; ++c) {
            if (c == ColKind) {
                auto *combo = new QComboBox(table_);
                combo->addItems(kKinds);
                combo->setCurrentText(segment.kind);
                connect(combo, &QComboBox::currentTextChanged, this, [this, row](const QString &kind) {
                    handleKindChanged(row, kind);
                });
                table_->setCellWidget(row, c, combo);
                continue;
            }
            auto *item = new QTableWidgetItem();
            Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (c == ColTitle || c == ColComment) {
                flags |= Qt::ItemIsEditable;
            } else if (c == ColEnabled) {
                flags |= Qt::ItemIsUserCheckable;
            }
            item->setFlags(flags);
            table_->setItem(row, c, item);
        }
        refreshRow(row);
    }
    if (previousRow >= 0 && previousRow < table_->rowCount()) {
        table_->selectRow(previousRow);
    }
    applyingState_ = false;
}

void SegmentsViewerDialog::refreshRow(int row)
{
    if (row < 0 || row >= segments_.size() || row >= table_->rowCount()) {
        return;
    }
    const QSignalBlocker blocker(table_);
    const bool wasApplying = applyingState_;
    applyingState_ = true;
    const TbcMetaData::Segment &segment = segments_.at(row);
    // Frame range: from the state when the segment is unchanged, else unknown
    QString frames = QStringLiteral("—");
    for (const SegmentsViewerRow &stateRow : state_.rows) {
        if (stateRow.segment.id == segment.id && stateRow.segment.startField == segment.startField
            && stateRow.segment.endFieldExclusive == segment.endFieldExclusive) {
            frames = stateRow.hasFrames
                         ? QStringLiteral("%1–%2").arg(stateRow.startFrame).arg(stateRow.startFrame + stateRow.lengthFrames - 1)
                         : tr("none");
            break;
        }
    }
    auto setText = [this, row](int column, const QString &text) {
        if (QTableWidgetItem *item = table_->item(row, column)) {
            item->setText(text);
        }
    };
    setText(ColId, QString::number(segment.id));
    setText(ColStart, QString::number(segment.startField));
    setText(ColEnd, QString::number(segment.endFieldExclusive));
    setText(ColFrames, frames);
    setText(ColDuration, fieldsToDuration(segment.endFieldExclusive - segment.startField));
    setText(ColSource, segment.source);
    if (QTableWidgetItem *item = table_->item(row, ColEnabled)) {
        item->setCheckState(segment.enabled ? Qt::Checked : Qt::Unchecked);
        item->setText(segment.enabled ? tr("yes") : tr("no"));
    }
    setText(ColTitle, segment.title);
    setText(ColComment, segment.comment);
    if (auto *combo = qobject_cast<QComboBox *>(table_->cellWidget(row, ColKind))) {
        const QSignalBlocker comboBlocker(combo);
        combo->setCurrentText(segment.kind);
    }
    applyingState_ = wasApplying;
}

void SegmentsViewerDialog::updateButtons()
{
    if (!applyButton_ || !rederiveButton_ || !table_) {
        return;
    }
    const int row = selectedRow();
    const bool hasRow = row >= 0;
    const bool hasCurrent = state_.currentFirstField >= 0;
    goStartButton_->setEnabled(hasRow);
    goEndButton_->setEnabled(hasRow);
    setInOutButton_->setEnabled(hasRow && !state_.metadataOnly);
    moveStartButton_->setEnabled(hasRow && hasCurrent);
    moveEndButton_->setEnabled(hasRow && hasCurrent);
    splitButton_->setEnabled(hasRow && hasCurrent
                             && state_.currentFirstField > segments_.at(row).startField
                             && state_.currentFirstField < segments_.at(row).endFieldExclusive);
    mergeButton_->setEnabled(hasRow && row + 1 < segments_.size());
    deleteButton_->setEnabled(hasRow);
    rederiveButton_->setEnabled(state_.totalFields > 0);
    applyButton_->setEnabled(dirty_);
    const bool custom = presetCombo_->currentIndex() == 3;
    minClipFieldsSpin_->setEnabled(custom);
    minNonClipRunSpin_->setEnabled(custom);
    noiseIreSpin_->setEnabled(custom);
    sceneIreSpin_->setEnabled(custom);
}

void SegmentsViewerDialog::updateInfoLabel()
{
    QString text;
    if (state_.totalFields <= 0) {
        text = tr("No source loaded.");
    } else if (segments_.isEmpty()) {
        text = state_.evidenceAvailable
                   ? tr("No segments. Re-derive to build them from the stored decoder events and picture metrics.")
                   : tr("No segments, and the metadata holds no decoder events or picture metrics to derive them from "
                        "(decode with vhs-decode schema 2, or backfill with tbc-segments --write).");
    } else {
        qint32 enabledCount = 0;
        qint32 userCount = 0;
        for (const TbcMetaData::Segment &segment : segments_) {
            enabledCount += segment.enabled ? 1 : 0;
            userCount += segment.source == QLatin1String("user") ? 1 : 0;
        }
        text = tr("%1 segments, %2 enabled, %3 user-edited.").arg(segments_.size()).arg(enabledCount).arg(userCount);
        if (state_.derivedAtLoad) {
            text += tr(" Derived when the file was opened; Save Metadata stores them.");
        }
        if (dirty_) {
            text += tr(" Unapplied edits.");
        }
    }
    infoLabel_->setText(text);
}

int SegmentsViewerDialog::selectedRow() const
{
    const int row = table_->currentRow();
    return (row >= 0 && row < segments_.size()) ? row : -1;
}

void SegmentsViewerDialog::markUserEdited(TbcMetaData::Segment &segment)
{
    segment.source = QStringLiteral("user");
    segment.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (segment.createdBy.isEmpty()) {
        segment.createdBy = QStringLiteral("ld-analyse");
    }
    dirty_ = true;
}

qint32 SegmentsViewerDialog::nextSegmentId() const
{
    qint32 maxId = 0;
    for (const TbcMetaData::Segment &segment : segments_) {
        maxId = qMax(maxId, segment.id);
    }
    for (const SegmentsViewerRow &row : state_.rows) {
        maxId = qMax(maxId, row.segment.id);
    }
    return maxId + 1;
}

void SegmentsViewerDialog::handleItemChanged(int row, int column)
{
    if (applyingState_ || row < 0 || row >= segments_.size()) {
        return;
    }
    QTableWidgetItem *item = table_->item(row, column);
    if (!item) {
        return;
    }
    TbcMetaData::Segment &segment = segments_[row];
    if (column == ColTitle) {
        if (segment.title == item->text().trimmed()) return;
        segment.title = item->text().trimmed();
    } else if (column == ColComment) {
        if (segment.comment == item->text().trimmed()) return;
        segment.comment = item->text().trimmed();
    } else if (column == ColEnabled) {
        const bool enabled = item->checkState() == Qt::Checked;
        if (segment.enabled == enabled) return;
        segment.enabled = enabled;
    } else {
        return;
    }
    markUserEdited(segment);
    refreshRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::handleKindChanged(int row, const QString &kind)
{
    if (applyingState_ || row < 0 || row >= segments_.size() || segments_.at(row).kind == kind) {
        return;
    }
    segments_[row].kind = kind;
    markUserEdited(segments_[row]);
    refreshRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::moveStartHere()
{
    const int row = selectedRow();
    const qint32 field = state_.currentFirstField;
    if (row < 0 || field < 0) {
        return;
    }
    TbcMetaData::Segment &segment = segments_[row];
    if (field >= segment.endFieldExclusive) {
        QMessageBox::information(this, tr("Move Start"), tr("The current frame is not before the segment's end."));
        return;
    }
    if (row > 0 && field <= segments_.at(row - 1).startField) {
        QMessageBox::information(this, tr("Move Start"), tr("The current frame is inside an earlier segment."));
        return;
    }
    const qint32 oldStart = segment.startField;
    segment.startField = field;
    markUserEdited(segment);
    // A shared seam moves the neighbour with it
    if (row > 0 && segments_.at(row - 1).endFieldExclusive == oldStart) {
        segments_[row - 1].endFieldExclusive = field;
        markUserEdited(segments_[row - 1]);
        refreshRow(row - 1);
    }
    refreshRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::moveEndHere()
{
    const int row = selectedRow();
    if (row < 0 || state_.currentFirstField < 0) {
        return;
    }
    // The end is exclusive: the current frame's two fields stay inside the segment
    const qint32 field = qMin(state_.totalFields, state_.currentFirstField + 2);
    TbcMetaData::Segment &segment = segments_[row];
    if (field <= segment.startField) {
        QMessageBox::information(this, tr("Move End"), tr("The current frame is not after the segment's start."));
        return;
    }
    if (row + 1 < segments_.size() && field >= segments_.at(row + 1).endFieldExclusive) {
        QMessageBox::information(this, tr("Move End"), tr("The current frame is beyond the next segment."));
        return;
    }
    const qint32 oldEnd = segment.endFieldExclusive;
    segment.endFieldExclusive = field;
    markUserEdited(segment);
    if (row + 1 < segments_.size() && segments_.at(row + 1).startField == oldEnd) {
        segments_[row + 1].startField = field;
        markUserEdited(segments_[row + 1]);
        refreshRow(row + 1);
    }
    refreshRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::splitAtCurrentFrame()
{
    const int row = selectedRow();
    const qint32 field = state_.currentFirstField;
    if (row < 0 || field <= segments_.at(row).startField || field >= segments_.at(row).endFieldExclusive) {
        return;
    }
    TbcMetaData::Segment head = segments_.at(row);
    TbcMetaData::Segment tail = head;
    head.endFieldExclusive = field;
    markUserEdited(head);
    tail.id = nextSegmentId();
    tail.startField = field;
    tail.title.clear();
    tail.comment.clear();
    tail.derivedFrom.clear();
    tail.createdBy = QStringLiteral("ld-analyse");
    markUserEdited(tail);
    segments_[row] = head;
    segments_.insert(row + 1, tail);
    rebuildTable();
    table_->selectRow(row + 1);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::mergeWithNext()
{
    const int row = selectedRow();
    if (row < 0 || row + 1 >= segments_.size()) {
        return;
    }
    TbcMetaData::Segment &segment = segments_[row];
    segment.endFieldExclusive = qMax(segment.endFieldExclusive, segments_.at(row + 1).endFieldExclusive);
    if (segment.title.isEmpty()) {
        segment.title = segments_.at(row + 1).title;
    }
    markUserEdited(segment);
    segments_.removeAt(row + 1);
    rebuildTable();
    table_->selectRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::deleteSelected()
{
    const int row = selectedRow();
    if (row < 0) {
        return;
    }
    segments_.removeAt(row);
    dirty_ = true;
    rebuildTable();
    if (table_->rowCount() > 0) {
        table_->selectRow(qMin(row, table_->rowCount() - 1));
    }
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::applyPreset(int index)
{
    SegmentsThresholds thresholds;
    if (index == 0) {
        thresholds = SegmentsThresholds::preset(SegmentSensitivity::Low);
    } else if (index == 2) {
        thresholds = SegmentsThresholds::preset(SegmentSensitivity::High);
    } else if (index == 1) {
        thresholds = SegmentsThresholds::preset(SegmentSensitivity::Normal);
    } else {
        updateButtons();
        return;
    }
    const QSignalBlocker b1(minClipFieldsSpin_);
    const QSignalBlocker b2(minNonClipRunSpin_);
    const QSignalBlocker b3(noiseIreSpin_);
    const QSignalBlocker b4(sceneIreSpin_);
    minClipFieldsSpin_->setValue(thresholds.minClipFields);
    minNonClipRunSpin_->setValue(thresholds.minNonClipRunFields);
    noiseIreSpin_->setValue(thresholds.noiseThresholdIre);
    sceneIreSpin_->setValue(thresholds.sceneThresholdIre);
    updateButtons();
}

void SegmentsViewerDialog::rederive()
{
    SegmentsThresholds thresholds;
    const int index = presetCombo_->currentIndex();
    if (index == 0) {
        thresholds = SegmentsThresholds::preset(SegmentSensitivity::Low);
    } else if (index == 2) {
        thresholds = SegmentsThresholds::preset(SegmentSensitivity::High);
    } else if (index == 1) {
        thresholds = SegmentsThresholds::preset(SegmentSensitivity::Normal);
    } else {
        thresholds.minClipFields = minClipFieldsSpin_->value();
        thresholds.minNonClipRunFields = minNonClipRunSpin_->value();
        thresholds.noiseThresholdIre = noiseIreSpin_->value();
        thresholds.sceneThresholdIre = sceneIreSpin_->value();
    }
    if (dirty_) {
        emit segmentsUpdated(segments_);
        dirty_ = false;
    }
    emit rederiveRequested(thresholds);
}
