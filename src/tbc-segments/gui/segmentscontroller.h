/************************************************************************

    segmentscontroller.h

    tbc-segments - tbc-analyse's recording-segment behaviour
    Copyright (C) 2026 GDH-Technologies LLC

    This file is part of tbc-tools.

    tbc-segments is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#ifndef SEGMENTSCONTROLLER_H
#define SEGMENTSCONTROLLER_H

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "segments.h"
#include "tbcmetadata.h"

class QAction;
class QMenu;
class QWidget;
class SegmentsViewerDialog;

// A tinted span of the timeline (slider positions, inclusive).
struct SegmentTimelineSpan {
    qint32 startPosition = -1;
    qint32 endPosition = -1;
    QColor color;
};

// Everything tbc-analyse does with recording segments: the Segments viewer,
// edits (apply, split, re-derive), set In/Out from a segment, the timeline's
// boundary ticks and tints, chapter-skip targets, the status readout and the
// timeline context menu. The main window supplies its source and view through
// Hooks and keeps only the calls, so the segment behaviour lives in one module.
class SegmentsController : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        // Source state
        std::function<bool()> isLoaded;
        std::function<bool()> isMetadataOnly;
        std::function<qint32()> numberOfFrames;
        std::function<qint32()> numberOfFields;
        std::function<const QVector<TbcMetaData::Segment> &()> segments;
        std::function<void(const QVector<TbcMetaData::Segment> &)> setSegments;
        std::function<QVector<TbcMetaData::Segment>(const SegmentsThresholds &)> deriveSegments;
        std::function<bool()> derivedAtLoad;
        std::function<bool()> evidenceAvailable;
        std::function<bool(const TbcMetaData::Segment &, qint32 *, qint32 *)> segmentFrameRange;
        std::function<qint32(qint32)> frameContainingField;
        // View
        std::function<qint32()> currentFrame;
        std::function<qint32()> currentFirstField;      // 0-based first field of the current frame
        std::function<qint32(qint32)> sliderPositionForField;
        std::function<QString(qint32)> frameToTimecode;
        std::function<double()> timecodeFrameRate;
        std::function<qint32()> timecodeFrameBaseRate;
        std::function<void(qint32)> goToField;
        std::function<void(QWidget *)> showOrRaise;
        std::function<void(const QString &)> showStatus;
        // Export In/Out (absent when there is no export dialog)
        std::function<bool()> canSetInOut;
        std::function<void(qint32)> setInPoint;
        std::function<void(qint32)> setOutPoint;
        // After the segment layer changed: mark modified, refresh panels
        std::function<void()> segmentsEdited;
    };

    SegmentsController(QWidget *parentWindow, Hooks hooks);

    SegmentsViewerDialog *viewer() const { return viewer_; }

    bool hasSegments() const;
    qint32 segmentIndexContainingField(qint32 field) const;
    qint32 segmentStartFrame(const TbcMetaData::Segment &segment) const;
    QString segmentSummaryText(const TbcMetaData::Segment &segment) const;

    // Edits
    void applySegmentEdit(const QVector<TbcMetaData::Segment> &segments, const QString &statusText);
    void setInOutFromSegment(qint32 segmentIndex, bool setIn, bool setOut);
    void setInOutFromSegmentAtCurrentFrame(bool setIn, bool setOut);
    bool splitSegmentAtField(qint32 field, QString *statusText);
    void rederiveSegments(const SegmentsThresholds &thresholds);

    // Viewer
    void refreshViewer();
    void refreshViewerIfVisible();
    void showViewer();

    // Timeline: a tick per boundary (with a tooltip), a tint over noise,
    // blank and disabled spans
    void timelineMarkers(QVector<qint32> *boundaryPositions, QStringList *boundaryTooltips,
                         QVector<SegmentTimelineSpan> *spans) const;
    // Chapter skip by segment start frame
    qint32 nextSegmentStartFrame(qint32 currentFrame, qint32 totalFrames) const;
    qint32 previousSegmentStartFrame(qint32 currentFrame) const;
    // " Seg: i/N" for the bottom readout (empty without segments)
    QString statusReadout(qint32 firstFieldOneBased) const;

    // Timeline context menu: adds the segment items for the clicked frame and
    // returns the handler for the chosen action (true when it was one of them)
    std::function<bool(QAction *)> addTimelineMenuActions(QMenu *menu, qint32 framePoint, const QString &framePointTimecode,
                                                          qint32 clickedFirstField);

private:
    QWidget *parentWindow_ = nullptr;
    Hooks hooks_;
    SegmentsViewerDialog *viewer_ = nullptr;
};

#endif // SEGMENTSCONTROLLER_H
