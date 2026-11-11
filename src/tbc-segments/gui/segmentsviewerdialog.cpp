/******************************************************************************
 * segmentsviewerdialog.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/
#include "segmentsviewerdialog.h"

#include "glossary_generated.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtMath>

namespace {

// Glossary: segment (docs/Tools/tbc-segments-glossary.md#segment): the stored kinds
const QStringList kKinds = {contentSpanKind(), QStringLiteral("blank"), QStringLiteral("noise"), QStringLiteral("unknown")};
const QStringList kVerdicts = {QStringLiteral("detected"), QStringLiteral("confirmed"), QStringLiteral("rejected")};

// A kind as shown: a stored legacy name reads as its current one
QString shownKind(const QString &kind)
{
    return isContentSpanKind(kind) ? contentSpanKind() : kind;
}

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

    tabs_ = new QTabWidget(this);
    auto *segmentsPage = new QWidget(tabs_);
    auto *segmentsLayout = new QVBoxLayout(segmentsPage);
    segmentsLayout->setContentsMargins(0, 0, 0, 0);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setObjectName(QStringLiteral("segmentsTable"));
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
    segmentsLayout->addWidget(table_, 1);

    auto *navLayout = new QHBoxLayout();
    goStartButton_ = new QPushButton(tr("Go To Start"), this);
    goEndButton_ = new QPushButton(tr("Go To End"), this);
    setInOutButton_ = new QPushButton(tr("Set In/Out From Segment"), this);
    for (QPushButton *button : {goStartButton_, goEndButton_, setInOutButton_}) {
        button->setAutoDefault(false);
        navLayout->addWidget(button);
    }
    navLayout->addStretch(1);
    segmentsLayout->addLayout(navLayout);

    auto *editLayout = new QHBoxLayout();
    moveStartButton_ = new QPushButton(tr("Move Start Here"), this);
    moveEndButton_ = new QPushButton(tr("Move End Here"), this);
    cutButton_ = new QPushButton(tr("Cut At Current Frame"), this);
    cutButton_->setToolTip(Glossary::tooltip(Glossary::Term::Cut));
    mergeButton_ = new QPushButton(tr("Merge With Next"), this);
    deleteButton_ = new QPushButton(tr("Delete"), this);
    for (QPushButton *button : {moveStartButton_, moveEndButton_, cutButton_, mergeButton_, deleteButton_}) {
        button->setAutoDefault(false);
        editLayout->addWidget(button);
    }
    editLayout->addStretch(1);
    segmentsLayout->addLayout(editLayout);
    tabs_->addTab(segmentsPage, tr("Segments"));

    // Shot boundaries inside the content spans; scenes and shots are worked
    // out from them (Glossary: shot boundary, scene change, verdict)
    auto *boundaryPage = new QWidget(tabs_);
    auto *boundaryLayout = new QVBoxLayout(boundaryPage);
    boundaryLayout->setContentsMargins(0, 0, 0, 0);
    boundaryTable_ = new QTableWidget(0, BColCount, boundaryPage);
    boundaryTable_->setObjectName(QStringLiteral("shotBoundaryTable"));
    boundaryTable_->setHorizontalHeaderLabels({tr("#"), tr("Time"), tr("Frame"), tr("Evidence"), tr("Confidence"),
                                               tr("Reason"), tr("Source"), tr("Verdict"), tr("Scene Change"),
                                               tr("Scene Title"), tr("Scene Enabled"), tr("Shot Enabled"), tr("Comment")});
    const QVector<QPair<int, Glossary::Term>> headerTerms = {
        {BColEvidence, Glossary::Term::Evidence}, {BColConfidence, Glossary::Term::Confidence},
        {BColSource, Glossary::Term::Source}, {BColVerdict, Glossary::Term::Verdict},
        {BColSceneChange, Glossary::Term::SceneChange}, {BColSceneTitle, Glossary::Term::Title},
        {BColSceneEnabled, Glossary::Term::Enabled}, {BColShotEnabled, Glossary::Term::Enabled}};
    for (const auto &header : headerTerms) {
        if (QTableWidgetItem *item = boundaryTable_->horizontalHeaderItem(header.first)) {
            item->setToolTip(Glossary::tooltip(header.second));
        }
    }
    boundaryTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    boundaryTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    boundaryTable_->verticalHeader()->setVisible(false);
    for (int c = 0; c < BColCount; ++c) {
        boundaryTable_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    boundaryTable_->horizontalHeader()->setSectionResizeMode(BColReason, QHeaderView::Stretch);
    boundaryTable_->horizontalHeader()->setSectionResizeMode(BColComment, QHeaderView::Stretch);
    boundaryLayout->addWidget(boundaryTable_, 1);
    auto *boundaryButtons = new QHBoxLayout();
    goBoundaryButton_ = new QPushButton(tr("Go To Shot Boundary"), boundaryPage);
    addBoundaryButton_ = new QPushButton(tr("Add Shot Boundary At Current Frame"), boundaryPage);
    addBoundaryButton_->setToolTip(Glossary::tooltip(Glossary::Term::ShotBoundary));
    for (QPushButton *button : {goBoundaryButton_, addBoundaryButton_}) {
        button->setAutoDefault(false);
        boundaryButtons->addWidget(button);
    }
    boundaryButtons->addStretch(1);
    boundaryLayout->addLayout(boundaryButtons);
    tabs_->addTab(boundaryPage, tr("Shot Boundaries"));
    tabs_->setTabToolTip(1, Glossary::tooltip(Glossary::Term::ShotBoundary));
    mainLayout->addWidget(tabs_, 1);

    auto *deriveLayout = new QGridLayout();
    deriveLayout->addWidget(new QLabel(tr("Re-derive"), this), 0, 0);
    presetCombo_ = new QComboBox(this);
    presetCombo_->addItems({tr("Low sensitivity"), tr("Normal"), tr("High sensitivity"), tr("Custom")});
    presetCombo_->setCurrentIndex(1);
    deriveLayout->addWidget(presetCombo_, 0, 1);
    deriveLayout->addWidget(new QLabel(tr("Min content span fields"), this), 0, 2);
    minContentSpanFieldsSpin_ = new QSpinBox(this);
    minContentSpanFieldsSpin_->setRange(1, 100000);
    minContentSpanFieldsSpin_->setToolTip(Glossary::tooltip(Glossary::Term::ContentSpan));
    deriveLayout->addWidget(minContentSpanFieldsSpin_, 0, 3);
    deriveLayout->addWidget(new QLabel(tr("Min non-content run"), this), 0, 4);
    minNonContentRunSpin_ = new QSpinBox(this);
    minNonContentRunSpin_->setRange(1, 100000);
    deriveLayout->addWidget(minNonContentRunSpin_, 0, 5);
    deriveLayout->addWidget(new QLabel(tr("Noise IRE"), this), 0, 6);
    noiseIreSpin_ = new QDoubleSpinBox(this);
    noiseIreSpin_->setRange(0.1, 100.0);
    noiseIreSpin_->setDecimals(1);
    deriveLayout->addWidget(noiseIreSpin_, 0, 7);
    deriveLayout->addWidget(new QLabel(tr("Spike IRE"), this), 0, 8);
    fieldDiffSpikeIreSpin_ = new QDoubleSpinBox(this);
    fieldDiffSpikeIreSpin_->setRange(0.1, 100.0);
    fieldDiffSpikeIreSpin_->setDecimals(1);
    fieldDiffSpikeIreSpin_->setToolTip(Glossary::tooltip(Glossary::Term::FieldDiffSpike));
    deriveLayout->addWidget(fieldDiffSpikeIreSpin_, 0, 9);
    rederiveButton_ = new QPushButton(tr("Re-derive"), this);
    rederiveButton_->setAutoDefault(false);
    rederiveButton_->setToolTip(tr("Derive segments again from the stored decoder events and picture metrics. "
                                   "User-edited segments are kept; derived segments they overlap are dropped."));
    deriveLayout->addWidget(rederiveButton_, 0, 10);
    deriveLayout->setColumnStretch(11, 1);
    mainLayout->addLayout(deriveLayout);

    // No default button: Enter belongs to the table's editors
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    applyButton_ = buttonBox->button(QDialogButtonBox::Apply);
    applyButton_->setAutoDefault(false);
    buttonBox->button(QDialogButtonBox::Close)->setAutoDefault(false);
    mainLayout->addWidget(buttonBox);

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
    connect(cutButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::cutAtCurrentFrame);
    connect(mergeButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::mergeWithNext);
    connect(deleteButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::deleteSelected);
    connect(presetCombo_, &QComboBox::currentIndexChanged, this, &SegmentsViewerDialog::applyPreset);
    connect(rederiveButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::rederive);
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        flushUnappliedEdits();
        updateButtons();
    });
    connect(boundaryTable_, &QTableWidget::itemSelectionChanged, this, &SegmentsViewerDialog::updateButtons);
    connect(boundaryTable_, &QTableWidget::cellChanged, this, &SegmentsViewerDialog::handleBoundaryItemChanged);
    connect(boundaryTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (row < 0 || row >= boundaries_.size() || column >= BColVerdict) {
            return;
        }
        emit goToFieldRequested(boundaries_.at(row).field);
    });
    connect(goBoundaryButton_, &QPushButton::clicked, this, [this]() {
        const int row = selectedBoundaryRow();
        if (row >= 0) {
            emit goToFieldRequested(boundaries_.at(row).field);
        }
    });
    connect(addBoundaryButton_, &QPushButton::clicked, this, &SegmentsViewerDialog::addShotBoundaryAtCurrentFrame);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

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
    if (!boundariesDirty_) {
        if (!(state.shotBoundaries == boundaries_) || boundaryTable_->rowCount() != boundaries_.size()) {
            boundaries_ = state.shotBoundaries;
            rebuildBoundaryTable();
        } else {
            for (int row = 0; row < boundaries_.size(); ++row) refreshBoundaryRow(row);
        }
    } else {
        for (int row = 0; row < boundaries_.size(); ++row) refreshBoundaryRow(row);
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
                combo->setCurrentText(shownKind(segment.kind));
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
        combo->setCurrentText(shownKind(segment.kind));
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
    // Move Start/End only where the move is valid (the same rules the moves
    // themselves check): the new start must stay before the segment's end and
    // after the previous segment's start; the new (exclusive) end, which keeps
    // the current frame's two fields inside, must stay after the segment's
    // start and before the next segment's end.
    bool canMoveStart = false;
    bool canMoveEnd = false;
    if (hasRow && hasCurrent) {
        const TbcMetaData::Segment &segment = segments_.at(row);
        const qint32 startField = state_.currentFirstField;
        canMoveStart = startField < segment.endFieldExclusive
                       && !(row > 0 && startField <= segments_.at(row - 1).startField);
        const qint32 endField = qMin(state_.totalFields, state_.currentFirstField + 2);
        canMoveEnd = endField > segment.startField
                     && !(row + 1 < segments_.size() && endField >= segments_.at(row + 1).endFieldExclusive);
    }
    moveStartButton_->setEnabled(canMoveStart);
    moveEndButton_->setEnabled(canMoveEnd);
    cutButton_->setEnabled(hasRow && hasCurrent
                             && state_.currentFirstField > segments_.at(row).startField
                             && state_.currentFirstField < segments_.at(row).endFieldExclusive);
    mergeButton_->setEnabled(hasRow && row + 1 < segments_.size());
    deleteButton_->setEnabled(hasRow);
    rederiveButton_->setEnabled(state_.totalFields > 0);
    applyButton_->setEnabled(dirty_ || boundariesDirty_);
    if (goBoundaryButton_) {
        goBoundaryButton_->setEnabled(selectedBoundaryRow() >= 0);
        bool canAdd = hasCurrent;
        for (const ShotBoundary &boundary : boundaries_) {
            if (qAbs(boundary.field - state_.currentFirstField) <= 2) canAdd = false;
        }
        addBoundaryButton_->setEnabled(canAdd);
    }
    const bool custom = presetCombo_->currentIndex() == 3;
    minContentSpanFieldsSpin_->setEnabled(custom);
    minNonContentRunSpin_->setEnabled(custom);
    noiseIreSpin_->setEnabled(custom);
    fieldDiffSpikeIreSpin_->setEnabled(custom);
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
        if (!boundaries_.isEmpty()) {
            qint32 active = 0, sceneChanges = 0;
            for (const ShotBoundary &boundary : boundaries_) {
                if (boundary.rejected()) continue;
                active++;
                sceneChanges += boundary.sceneChange ? 1 : 0;
            }
            text += tr(" %1 shot boundaries, %2 marked scene changes.").arg(active).arg(sceneChanges);
        }
        if (dirty_ || boundariesDirty_) {
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
        segment.createdBy = QStringLiteral("tbc-analyse");
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
    // updateButtons() disables Move Start where these fail
    if (field >= segment.endFieldExclusive || (row > 0 && field <= segments_.at(row - 1).startField)) {
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
    // updateButtons() disables Move End where these fail
    if (field <= segment.startField
        || (row + 1 < segments_.size() && field >= segments_.at(row + 1).endFieldExclusive)) {
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

void SegmentsViewerDialog::cutAtCurrentFrame()
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
    tail.createdBy = QStringLiteral("tbc-analyse");
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
    const QSignalBlocker b1(minContentSpanFieldsSpin_);
    const QSignalBlocker b2(minNonContentRunSpin_);
    const QSignalBlocker b3(noiseIreSpin_);
    const QSignalBlocker b4(fieldDiffSpikeIreSpin_);
    minContentSpanFieldsSpin_->setValue(thresholds.minContentSpanFields);
    minNonContentRunSpin_->setValue(thresholds.minNonContentRunFields);
    noiseIreSpin_->setValue(thresholds.noiseThresholdIre);
    fieldDiffSpikeIreSpin_->setValue(thresholds.fieldDiffSpikeIre);
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
        thresholds.minContentSpanFields = minContentSpanFieldsSpin_->value();
        thresholds.minNonContentRunFields = minNonContentRunSpin_->value();
        thresholds.noiseThresholdIre = noiseIreSpin_->value();
        thresholds.fieldDiffSpikeIre = fieldDiffSpikeIreSpin_->value();
    }
    flushUnappliedEdits();
    emit rederiveRequested(thresholds);
}

// Unapplied edits go to the main window before anything that re-derives
void SegmentsViewerDialog::flushUnappliedEdits()
{
    if (dirty_) {
        dirty_ = false;
        emit segmentsUpdated(segments_);
    }
    if (boundariesDirty_) {
        boundariesDirty_ = false;
        emit shotBoundariesUpdated(boundaries_);
    }
}

void SegmentsViewerDialog::rebuildBoundaryTable()
{
    const bool wasApplying = applyingState_;
    applyingState_ = true;
    const QSignalBlocker blocker(boundaryTable_);
    const int previousRow = boundaryTable_->currentRow();
    boundaryTable_->setRowCount(0);
    boundaryTable_->setRowCount(boundaries_.size());
    for (int row = 0; row < boundaries_.size(); ++row) {
        for (int c = 0; c < BColCount; ++c) {
            if (c == BColVerdict) {
                auto *combo = new QComboBox(boundaryTable_);
                combo->addItems(kVerdicts);
                combo->setToolTip(Glossary::tooltip(Glossary::Term::Verdict));
                connect(combo, &QComboBox::currentTextChanged, this, [this, row](const QString &verdict) {
                    handleVerdictChanged(row, verdict);
                });
                boundaryTable_->setCellWidget(row, c, combo);
                continue;
            }
            auto *item = new QTableWidgetItem();
            Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
            if (c == BColComment || c == BColSceneTitle) {
                flags |= Qt::ItemIsEditable;
            } else if (c == BColSceneChange || c == BColSceneEnabled || c == BColShotEnabled) {
                flags |= Qt::ItemIsUserCheckable;
            }
            item->setFlags(flags);
            boundaryTable_->setItem(row, c, item);
        }
        refreshBoundaryRow(row);
    }
    if (previousRow >= 0 && previousRow < boundaryTable_->rowCount()) {
        boundaryTable_->selectRow(previousRow);
    }
    applyingState_ = wasApplying;
}

void SegmentsViewerDialog::refreshBoundaryRow(int row)
{
    if (row < 0 || row >= boundaries_.size() || row >= boundaryTable_->rowCount()) {
        return;
    }
    const QSignalBlocker blocker(boundaryTable_);
    const bool wasApplying = applyingState_;
    applyingState_ = true;
    const ShotBoundary &boundary = boundaries_.at(row);
    qint32 frame = -1;
    for (int i = 0; i < state_.shotBoundaries.size() && i < state_.shotBoundaryFrames.size(); ++i) {
        if (state_.shotBoundaries.at(i).field == boundary.field) {
            frame = state_.shotBoundaryFrames.at(i);
            break;
        }
    }
    if (frame < 1) {
        frame = boundary.field / 2 + 1;
    }
    auto setText = [this, row](int column, const QString &text) {
        if (QTableWidgetItem *item = boundaryTable_->item(row, column)) {
            item->setText(text);
        }
    };
    auto setCheck = [this, row](int column, bool checked) {
        if (QTableWidgetItem *item = boundaryTable_->item(row, column)) {
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            item->setText(checked ? tr("yes") : tr("no"));
        }
    };
    setText(BColId, QString::number(boundary.id));
    setText(BColTime, frameToTimecode(frame));
    setText(BColFrame, QString::number(frame));
    setText(BColEvidence, boundary.evidence);
    setText(BColConfidence, QStringLiteral("%1%").arg(qRound(boundary.confidence * 100.0)));
    setText(BColReason, boundary.reason);
    if (QTableWidgetItem *item = boundaryTable_->item(row, BColReason)) {
        item->setToolTip(boundary.reason);
    }
    setText(BColSource, boundary.source);
    setCheck(BColSceneChange, boundary.sceneChange);
    setText(BColSceneTitle, boundary.sceneTitle);
    setCheck(BColSceneEnabled, boundary.sceneEnabled);
    setCheck(BColShotEnabled, boundary.shotEnabled);
    setText(BColComment, boundary.comment);
    if (auto *combo = qobject_cast<QComboBox *>(boundaryTable_->cellWidget(row, BColVerdict))) {
        const QSignalBlocker comboBlocker(combo);
        combo->setCurrentText(boundary.verdict);
    }
    applyingState_ = wasApplying;
}

int SegmentsViewerDialog::selectedBoundaryRow() const
{
    if (!boundaryTable_) return -1;
    const int row = boundaryTable_->currentRow();
    return (row >= 0 && row < boundaries_.size()) ? row : -1;
}

// An edited boundary is a person's: a re-derive keeps it as it is. Any edit
// short of rejecting it is also a review that it is real.
void SegmentsViewerDialog::boundaryUserEdited(ShotBoundary &boundary)
{
    boundary.source = QStringLiteral("user");
    boundary.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (boundary.createdBy.isEmpty()) {
        boundary.createdBy = QStringLiteral("tbc-analyse");
    }
    if (boundary.verdict == QLatin1String("detected")) {
        boundary.verdict = QStringLiteral("confirmed");
    }
    boundariesDirty_ = true;
}

void SegmentsViewerDialog::handleBoundaryItemChanged(int row, int column)
{
    if (applyingState_ || row < 0 || row >= boundaries_.size()) {
        return;
    }
    QTableWidgetItem *item = boundaryTable_->item(row, column);
    if (!item) {
        return;
    }
    ShotBoundary &boundary = boundaries_[row];
    const bool checked = item->checkState() == Qt::Checked;
    if (column == BColComment) {
        if (boundary.comment == item->text().trimmed()) return;
        boundary.comment = item->text().trimmed();
    } else if (column == BColSceneTitle) {
        if (boundary.sceneTitle == item->text().trimmed()) return;
        boundary.sceneTitle = item->text().trimmed();
    } else if (column == BColSceneChange) {
        if (boundary.sceneChange == checked) return;
        boundary.sceneChange = checked;
    } else if (column == BColSceneEnabled) {
        if (boundary.sceneEnabled == checked) return;
        boundary.sceneEnabled = checked;
    } else if (column == BColShotEnabled) {
        if (boundary.shotEnabled == checked) return;
        boundary.shotEnabled = checked;
    } else {
        return;
    }
    boundaryUserEdited(boundary);
    refreshBoundaryRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::handleVerdictChanged(int row, const QString &verdict)
{
    if (applyingState_ || row < 0 || row >= boundaries_.size() || boundaries_.at(row).verdict == verdict) {
        return;
    }
    ShotBoundary &boundary = boundaries_[row];
    boundaryUserEdited(boundary);
    boundary.verdict = verdict;   // after boundaryUserEdited, which confirms a detected one
    refreshBoundaryRow(row);
    updateInfoLabel();
    updateButtons();
}

void SegmentsViewerDialog::addShotBoundaryAtCurrentFrame()
{
    const qint32 field = state_.currentFirstField;
    if (field < 0) {
        return;
    }
    for (const ShotBoundary &existing : boundaries_) {
        if (qAbs(existing.field - field) <= 2) return;
    }
    qint32 maxId = 0;
    for (const ShotBoundary &existing : boundaries_) maxId = qMax(maxId, existing.id);
    for (const ShotBoundary &existing : state_.shotBoundaries) maxId = qMax(maxId, existing.id);
    ShotBoundary boundary;
    boundary.id = maxId + 1;
    boundary.field = field;
    boundary.confidence = 1.0;
    boundary.reason = tr("added by hand");
    boundaryUserEdited(boundary);
    int insertAt = 0;
    while (insertAt < boundaries_.size() && boundaries_.at(insertAt).field < field) insertAt++;
    boundaries_.insert(insertAt, boundary);
    rebuildBoundaryTable();
    boundaryTable_->selectRow(insertAt);
    updateInfoLabel();
    updateButtons();
}
