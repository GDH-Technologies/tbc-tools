/******************************************************************************
 * segmentsviewerdialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/
#ifndef SEGMENTSVIEWERDIALOG_H
#define SEGMENTSVIEWERDIALOG_H

#include <QDialog>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "segments.h"
#include "tbcmetadata.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

// One segment with the frames an export of it covers (mixed-frame rule),
// computed by the main window through TbcSource.
struct SegmentsViewerRow {
    TbcMetaData::Segment segment;
    bool hasFrames = false;
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
};

struct SegmentsViewerState {
    qint32 totalFrames = 0;
    qint32 totalFields = 0;
    double frameRate = 30000.0 / 1001.0;
    qint32 frameBaseRate = 30;
    qint32 currentFrame = -1;
    qint32 currentFirstField = -1;     // 0-based first field of the current frame
    bool metadataOnly = false;
    bool derivedAtLoad = false;
    bool evidenceAvailable = false;    // decoder events or picture metrics stored
    QVector<SegmentsViewerRow> rows;
};

// The Segments viewer: list, jump, set In/Out, enable/disable, rename,
// move a boundary, split/merge, delete, re-derive. Edits are applied to the
// metadata by the main window on Apply (the segment layer; decoder records
// stay immutable) and saved with Save Metadata.
class SegmentsViewerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SegmentsViewerDialog(QWidget *parent = nullptr);
    void setState(const SegmentsViewerState &state);

signals:
    void goToFieldRequested(qint32 field);
    void setInOutRequested(qint32 segmentIndex);
    void segmentsUpdated(const QVector<TbcMetaData::Segment> &segments);
    void rederiveRequested(const SegmentsThresholds &thresholds);

private:
    enum Column { ColId = 0, ColStart, ColEnd, ColFrames, ColDuration, ColKind, ColSource, ColEnabled, ColTitle, ColComment, ColCount };

    QString frameToTimecode(qint32 frameNumber) const;
    QString fieldsToDuration(qint32 fields) const;
    void rebuildTable();
    void refreshRow(int row);
    void updateButtons();
    void updateInfoLabel();
    int selectedRow() const;
    void markUserEdited(TbcMetaData::Segment &segment);
    qint32 nextSegmentId() const;
    void handleItemChanged(int row, int column);
    void handleKindChanged(int row, const QString &kind);
    void moveStartHere();
    void moveEndHere();
    void splitAtCurrentFrame();
    void mergeWithNext();
    void deleteSelected();
    void rederive();
    void applyPreset(int index);

    SegmentsViewerState state_;
    QVector<TbcMetaData::Segment> segments_;   // working copy
    bool dirty_ = false;
    bool applyingState_ = false;

    QTableWidget *table_ = nullptr;
    QLabel *infoLabel_ = nullptr;
    QPushButton *goStartButton_ = nullptr;
    QPushButton *goEndButton_ = nullptr;
    QPushButton *setInOutButton_ = nullptr;
    QPushButton *moveStartButton_ = nullptr;
    QPushButton *moveEndButton_ = nullptr;
    QPushButton *splitButton_ = nullptr;
    QPushButton *mergeButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QComboBox *presetCombo_ = nullptr;
    QSpinBox *minClipFieldsSpin_ = nullptr;
    QSpinBox *minNonClipRunSpin_ = nullptr;
    QDoubleSpinBox *noiseIreSpin_ = nullptr;
    QDoubleSpinBox *sceneIreSpin_ = nullptr;
    QPushButton *rederiveButton_ = nullptr;
    QPushButton *applyButton_ = nullptr;
};

#endif // SEGMENTSVIEWERDIALOG_H
