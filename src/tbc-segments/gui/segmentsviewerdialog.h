/******************************************************************************
 * segmentsviewerdialog.h
 * tbc-analyse - TBC output analysis GUI
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

#include "shotboundaries.h"
#include "segments.h"
#include "tbcmetadata.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

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
    // Shot boundaries, with the 1-based frame each falls in
    // (Glossary: shot boundary, docs/Tools/tbc-segments-glossary.md#shot-boundary)
    QVector<ShotBoundary> shotBoundaries;
    QVector<qint32> shotBoundaryFrames;
};

// The Segments viewer. Segments tab: list, jump, set In/Out, enable/disable,
// rename, move a boundary, cut/merge, delete, re-derive. Shot Boundaries tab:
// go to, verdict, scene change with the scene's title and enabled flag, the
// shot's enabled flag, comment, add by hand. Edits are applied by the
// controller on Apply and saved with Save Metadata; decoder records stay
// immutable. Vocabulary: docs/Tools/tbc-segments-glossary.md.
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
    void shotBoundariesUpdated(const QVector<ShotBoundary> &boundaries);

private:
    enum Column { ColId = 0, ColStart, ColEnd, ColFrames, ColDuration, ColKind, ColSource, ColEnabled, ColTitle, ColComment, ColCount };
    enum BoundaryColumn { BColId = 0, BColTime, BColFrame, BColEvidence, BColConfidence, BColReason, BColSource,
                          BColVerdict, BColSceneChange, BColSceneTitle, BColSceneEnabled, BColShotEnabled,
                          BColComment, BColCount };

    void rebuildBoundaryTable();
    void refreshBoundaryRow(int row);
    int selectedBoundaryRow() const;
    void boundaryUserEdited(ShotBoundary &boundary);
    void handleBoundaryItemChanged(int row, int column);
    void handleVerdictChanged(int row, const QString &verdict);
    void addShotBoundaryAtCurrentFrame();
    void flushUnappliedEdits();

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
    void cutAtCurrentFrame();
    void mergeWithNext();
    void deleteSelected();
    void rederive();
    void applyPreset(int index);

    SegmentsViewerState state_;
    QVector<TbcMetaData::Segment> segments_;   // working copy
    bool dirty_ = false;
    bool applyingState_ = false;

    QVector<ShotBoundary> boundaries_;         // working copy
    bool boundariesDirty_ = false;
    QTabWidget *tabs_ = nullptr;
    QTableWidget *boundaryTable_ = nullptr;
    QPushButton *goBoundaryButton_ = nullptr;
    QPushButton *addBoundaryButton_ = nullptr;
    QTableWidget *table_ = nullptr;
    QLabel *infoLabel_ = nullptr;
    QPushButton *goStartButton_ = nullptr;
    QPushButton *goEndButton_ = nullptr;
    QPushButton *setInOutButton_ = nullptr;
    QPushButton *moveStartButton_ = nullptr;
    QPushButton *moveEndButton_ = nullptr;
    QPushButton *cutButton_ = nullptr;
    QPushButton *mergeButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QComboBox *presetCombo_ = nullptr;
    QSpinBox *minContentSpanFieldsSpin_ = nullptr;
    QSpinBox *minNonContentRunSpin_ = nullptr;
    QDoubleSpinBox *noiseIreSpin_ = nullptr;
    QDoubleSpinBox *fieldDiffSpikeIreSpin_ = nullptr;
    QPushButton *rederiveButton_ = nullptr;
    QPushButton *applyButton_ = nullptr;
};

#endif // SEGMENTSVIEWERDIALOG_H
