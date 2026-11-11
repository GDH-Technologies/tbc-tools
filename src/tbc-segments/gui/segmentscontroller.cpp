/************************************************************************

    segmentscontroller.cpp

    tbc-segments - tbc-analyse's segment behaviour
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

#include "segmentscontroller.h"

#include "segmentsviewerdialog.h"
#include "glossary_generated.h"
#include "shotboundarystore.h"

#include <QAction>
#include <QDateTime>
#include <QMenu>
#include <algorithm>

SegmentsController::SegmentsController(QWidget *parentWindow, Hooks hooks)
    : QObject(parentWindow), parentWindow_(parentWindow), hooks_(std::move(hooks))
{
    // Segments viewer: the editable segment layer of the metadata
    viewer_ = new SegmentsViewerDialog(parentWindow);
    connect(viewer_, &SegmentsViewerDialog::goToFieldRequested, this, [this](qint32 field) { hooks_.goToField(field); });
    connect(viewer_, &SegmentsViewerDialog::setInOutRequested, this, [this](qint32 segmentIndex) {
        setInOutFromSegment(segmentIndex, true, true);
    });
    connect(viewer_, &SegmentsViewerDialog::segmentsUpdated, this,
            [this](const QVector<TbcMetaData::Segment> &segments) {
                if (!hooks_.isLoaded()) {
                    return;
                }
                applySegmentEdit(segments, tr("Segments updated (%1); Save Metadata stores them").arg(segments.size()));
                refreshViewer();
            });
    connect(viewer_, &SegmentsViewerDialog::rederiveRequested, this, &SegmentsController::rederiveSegments);
    connect(viewer_, &SegmentsViewerDialog::shotBoundariesUpdated, this, [this](const QVector<ShotBoundary> &boundaries) {
        applyShotBoundaries(boundaries, tr("Shot boundaries updated; Save Metadata stores them"));
    });
}

void SegmentsController::sourceLoaded()
{
    shotBoundaries_.clear();
    if (!hooks_.isLoaded()) {
        return;
    }
    QVector<ShotBoundary> stored;
    QString error;
    if (!ShotBoundaryStore::read(ShotBoundaryStore::databasePathFor(hooks_.metadataPath()), &stored, nullptr, &error)) {
        hooks_.showStatus(tr("Could not read the shot boundaries: %1").arg(error));
    }
    // None stored: detect them (they persist with the next save)
    shotBoundaries_ = stored.isEmpty() ? hooks_.deriveLayer(lastThresholds_, {}).shotBoundaries : stored;
}

void SegmentsController::sourceUnloaded()
{
    shotBoundaries_.clear();
}

bool SegmentsController::persistShotBoundaries(QString *error)
{
    if (!hooks_.isLoaded()) {
        return true;
    }
    const QString database = ShotBoundaryStore::databasePathFor(hooks_.metadataPath());
    if (database.isEmpty()) {
        if (error) *error = tr("the metadata has no .tbc.db to hold shot boundaries");
        return shotBoundaries_.isEmpty();
    }
    return ShotBoundaryStore::write(database, shotBoundaries_, nullptr, error);
}

// Shot boundaries never change the stored segments (scenes and shots are
// worked out from them), so an edit only flags the metadata modified
void SegmentsController::applyShotBoundaries(const QVector<ShotBoundary> &boundaries, const QString &statusText)
{
    if (!hooks_.isLoaded()) {
        return;
    }
    shotBoundaries_ = boundaries;
    hooks_.segmentsEdited();
    if (!statusText.isEmpty()) {
        hooks_.showStatus(statusText);
    }
    refreshViewer();
}

qint32 SegmentsController::shotBoundaryNearField(qint32 field) const
{
    for (qint32 i = 0; i < shotBoundaries_.size(); ++i) {
        if (qAbs(shotBoundaries_.at(i).field - field) <= 2) return i;
    }
    return -1;
}

void SegmentsController::shotBoundaryTicks(QVector<qint32> *positions, QStringList *tooltips) const
{
    for (const ShotBoundary &boundary : shotBoundaries_) {
        if (boundary.rejected()) continue;
        const qint32 position = hooks_.sliderPositionForField(boundary.field);
        if (position <= 0) continue;
        positions->append(position);
        tooltips->append(tr("Shot boundary %1 (%2%%3): %4")
                             .arg(boundary.id)
                             .arg(qRound(boundary.confidence * 100.0))
                             .arg(boundary.sceneChange ? tr(", scene change") : QString())
                             .arg(boundary.reason));
    }
}

bool SegmentsController::hasSegments() const
{
    return !hooks_.segments().isEmpty();
}

qint32 SegmentsController::segmentIndexContainingField(qint32 field) const
{
    const QVector<TbcMetaData::Segment> &segments = hooks_.segments();
    for (qint32 i = 0; i < segments.size(); ++i) {
        if (field >= segments.at(i).startField && field < segments.at(i).endFieldExclusive) {
            return i;
        }
    }
    return -1;
}

qint32 SegmentsController::segmentStartFrame(const TbcMetaData::Segment &segment) const
{
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
    if (hooks_.segmentFrameRange(segment, &startFrame, &lengthFrames)) {
        return startFrame;
    }
    const qint32 frame = hooks_.frameContainingField(segment.startField);
    return frame >= 1 ? frame : -1;
}

QString SegmentsController::segmentSummaryText(const TbcMetaData::Segment &segment) const
{
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
    QString range;
    if (hooks_.segmentFrameRange(segment, &startFrame, &lengthFrames)) {
        range = QStringLiteral("%1–%2").arg(hooks_.frameToTimecode(startFrame)).arg(hooks_.frameToTimecode(startFrame + lengthFrames - 1));
    } else {
        range = tr("no whole frame");
    }
    const QString title = segment.title.trimmed().isEmpty() ? segment.kind : segment.title.trimmed();
    return tr("Segment %1: %2 (%3)%4")
        .arg(segment.id)
        .arg(title)
        .arg(range)
        .arg(segment.enabled ? QString() : tr(" [disabled]"));
}

// The notesUpdated sequence, for segment edits: metadata, Save, timeline, panels
void SegmentsController::applySegmentEdit(const QVector<TbcMetaData::Segment> &segments, const QString &statusText)
{
    hooks_.setSegments(segments);
    hooks_.segmentsEdited();
    if (!statusText.isEmpty()) {
        hooks_.showStatus(statusText);
    }
}

void SegmentsController::setInOutFromSegment(qint32 segmentIndex, bool setIn, bool setOut)
{
    if (!hooks_.canSetInOut() || !hooks_.isLoaded() || hooks_.isMetadataOnly()) {
        return;
    }
    const QVector<TbcMetaData::Segment> &segments = hooks_.segments();
    if (segmentIndex < 0 || segmentIndex >= segments.size()) {
        return;
    }
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
    if (!hooks_.segmentFrameRange(segments.at(segmentIndex), &startFrame, &lengthFrames)) {
        hooks_.showStatus(tr("Segment %1 holds no whole frame").arg(segments.at(segmentIndex).id));
        return;
    }
    if (setIn) {
        hooks_.setInPoint(startFrame);
    }
    if (setOut) {
        hooks_.setOutPoint(startFrame + lengthFrames - 1);
    }
    hooks_.showStatus(tr("Export %1 set from segment %2 (frames %3–%4)")
                          .arg(setIn && setOut ? tr("In/Out") : setIn ? tr("In") : tr("Out"))
                          .arg(segments.at(segmentIndex).id)
                          .arg(startFrame)
                          .arg(startFrame + lengthFrames - 1));
}

void SegmentsController::setInOutFromSegmentAtCurrentFrame(bool setIn, bool setOut)
{
    const qint32 index = segmentIndexContainingField(hooks_.currentFirstField());
    if (index >= 0) {
        setInOutFromSegment(index, setIn, setOut);
    } else {
        hooks_.showStatus(tr("No segment at the current frame"));
    }
}

// Cut the segment containing `field` at that field (Glossary: cut, #cut): the
// head keeps its id, the tail becomes a new segment; both become user segments.
bool SegmentsController::cutSegmentAtField(qint32 field, QString *statusText)
{
    QVector<TbcMetaData::Segment> segments = hooks_.segments();
    const qint32 index = segmentIndexContainingField(field);
    if (index < 0 || field <= segments.at(index).startField) {
        if (statusText) {
            *statusText = tr("No segment to cut at field %1").arg(field);
        }
        return false;
    }
    qint32 maxId = 0;
    for (const TbcMetaData::Segment &segment : segments) {
        maxId = qMax(maxId, segment.id);
    }
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    TbcMetaData::Segment head = segments.at(index);
    TbcMetaData::Segment tail = head;
    head.endFieldExclusive = field;
    head.source = QStringLiteral("user");
    head.updatedAt = now;
    tail.id = maxId + 1;
    tail.startField = field;
    tail.title.clear();
    tail.comment.clear();
    tail.source = QStringLiteral("user");
    tail.createdBy = QStringLiteral("tbc-analyse");
    tail.updatedAt = now;
    tail.derivedFrom.clear();
    segments[index] = head;
    segments.insert(index + 1, tail);
    applySegmentEdit(segments, QString());
    if (statusText) {
        *statusText = tr("Segment %1 cut at field %2; segment %3 created").arg(head.id).arg(field).arg(tail.id);
    }
    return true;
}

// Derive again with the given thresholds. User-edited segments are kept and
// derived segments they overlap are dropped, so a re-derive never undoes an edit.
void SegmentsController::rederiveSegments(const SegmentsThresholds &thresholds)
{
    if (!hooks_.isLoaded()) {
        return;
    }
    QVector<TbcMetaData::Segment> kept;
    for (const TbcMetaData::Segment &segment : hooks_.segments()) {
        if (segment.source == QLatin1String("user")) {
            kept.append(segment);
        }
    }
    lastThresholds_ = thresholds;
    const SegmentLayer layer = hooks_.deriveLayer(thresholds, shotBoundaries_);
    shotBoundaries_ = layer.shotBoundaries;
    const QVector<TbcMetaData::Segment> &derived = layer.segments;
    qint32 nextId = 0;
    for (const TbcMetaData::Segment &segment : kept) {
        nextId = qMax(nextId, segment.id);
    }
    QVector<TbcMetaData::Segment> merged = kept;
    qint32 dropped = 0;
    for (TbcMetaData::Segment segment : derived) {
        bool overlapsUser = false;
        for (const TbcMetaData::Segment &user : kept) {
            if (segment.startField < user.endFieldExclusive && user.startField < segment.endFieldExclusive) {
                overlapsUser = true;
                break;
            }
        }
        if (overlapsUser) {
            dropped++;
            continue;
        }
        segment.id = ++nextId;
        merged.append(segment);
    }
    std::stable_sort(merged.begin(), merged.end(), [](const TbcMetaData::Segment &a, const TbcMetaData::Segment &b) {
        return a.startField < b.startField;
    });
    applySegmentEdit(merged, tr("Segments re-derived: %1 derived (%2 dropped for user segments), %3 user kept")
                                 .arg(derived.size() - dropped)
                                 .arg(dropped)
                                 .arg(kept.size()));
    refreshViewer();
}

void SegmentsController::refreshViewer()
{
    if (!viewer_) {
        return;
    }
    SegmentsViewerState state;
    state.frameRate = hooks_.timecodeFrameRate();
    state.frameBaseRate = hooks_.timecodeFrameBaseRate();
    if (hooks_.isLoaded()) {
        state.totalFrames = qMax<qint32>(1, hooks_.numberOfFrames());
        state.totalFields = qMax<qint32>(1, hooks_.numberOfFields());
        state.currentFrame = qBound<qint32>(1, hooks_.currentFrame(), state.totalFrames);
        state.currentFirstField = hooks_.currentFirstField();
        state.metadataOnly = hooks_.isMetadataOnly();
        state.derivedAtLoad = hooks_.derivedAtLoad();
        state.evidenceAvailable = hooks_.evidenceAvailable();
        const QVector<TbcMetaData::Segment> &segments = hooks_.segments();
        state.rows.reserve(segments.size());
        for (const TbcMetaData::Segment &segment : segments) {
            SegmentsViewerRow row;
            row.segment = segment;
            row.hasFrames = hooks_.segmentFrameRange(segment, &row.startFrame, &row.lengthFrames);
            state.rows.append(row);
        }
        state.shotBoundaries = shotBoundaries_;
        state.shotBoundaryFrames.reserve(shotBoundaries_.size());
        for (const ShotBoundary &boundary : shotBoundaries_) {
            state.shotBoundaryFrames.append(hooks_.frameContainingField(boundary.field));
        }
    }
    viewer_->setState(state);
}

void SegmentsController::refreshViewerIfVisible()
{
    if (viewer_ && viewer_->isVisible()) {
        refreshViewer();
    }
}

void SegmentsController::showViewer()
{
    if (!viewer_) {
        return;
    }
    refreshViewer();
    hooks_.showOrRaise(viewer_);
}

void SegmentsController::timelineMarkers(QVector<qint32> *boundaryPositions, QStringList *boundaryTooltips,
                                         QVector<SegmentTimelineSpan> *spans) const
{
    const QVector<TbcMetaData::Segment> &segments = hooks_.segments();
    for (qint32 i = 0; i < segments.size(); ++i) {
        const TbcMetaData::Segment &segment = segments.at(i);
        if (segment.startField > 0) {
            const qint32 position = hooks_.sliderPositionForField(segment.startField);
            if (position > 0) {
                boundaryPositions->append(position);
                boundaryTooltips->append(segmentSummaryText(segment));
            }
        }
        QColor tint;
        if (segment.kind == QLatin1String("noise")) {
            tint = QColor(255, 170, 0, 50);
        } else if (segment.kind == QLatin1String("blank")) {
            tint = QColor(128, 128, 128, 60);
        } else if (!segment.enabled) {
            tint = QColor(220, 45, 45, 40);
        }
        if (tint.isValid()) {
            SegmentTimelineSpan span;
            span.startPosition = hooks_.sliderPositionForField(segment.startField);
            span.endPosition = hooks_.sliderPositionForField(qMax(segment.startField, segment.endFieldExclusive - 1));
            span.color = tint;
            if (span.startPosition > 0 && span.endPosition >= span.startPosition) {
                spans->append(span);
            }
        }
    }
}

qint32 SegmentsController::nextSegmentStartFrame(qint32 currentFrame, qint32 totalFrames) const
{
    qint32 targetFrame = totalFrames;
    for (const TbcMetaData::Segment &segment : hooks_.segments()) {
        const qint32 startFrame = segmentStartFrame(segment);
        if (startFrame > currentFrame && startFrame < targetFrame) {
            targetFrame = startFrame;
        }
    }
    // Shot boundaries are stops of their own (Glossary: shot boundary, #shot-boundary)
    for (const ShotBoundary &boundary : shotBoundaries_) {
        if (boundary.rejected()) continue;
        const qint32 frame = hooks_.frameContainingField(boundary.field);
        if (frame > currentFrame && frame < targetFrame) {
            targetFrame = frame;
        }
    }
    return targetFrame;
}

qint32 SegmentsController::previousSegmentStartFrame(qint32 currentFrame) const
{
    qint32 targetFrame = 1;
    for (const TbcMetaData::Segment &segment : hooks_.segments()) {
        const qint32 startFrame = segmentStartFrame(segment);
        if (startFrame < currentFrame && startFrame > targetFrame) {
            targetFrame = startFrame;
        }
    }
    for (const ShotBoundary &boundary : shotBoundaries_) {
        if (boundary.rejected()) continue;
        const qint32 frame = hooks_.frameContainingField(boundary.field);
        if (frame < currentFrame && frame > targetFrame) {
            targetFrame = frame;
        }
    }
    return targetFrame;
}

QString SegmentsController::statusReadout(qint32 firstFieldOneBased) const
{
    const QVector<TbcMetaData::Segment> &segments = hooks_.segments();
    if (segments.isEmpty()) {
        return QString();
    }
    const qint32 index = segmentIndexContainingField(qMax<qint32>(0, firstFieldOneBased - 1));
    return index >= 0
               ? QStringLiteral(" Seg: %1/%2").arg(index + 1).arg(segments.size())
               : QStringLiteral(" Seg: -/%1").arg(segments.size());
}

std::function<bool(QAction *)> SegmentsController::addTimelineMenuActions(QMenu *menu, qint32 framePoint,
                                                                         const QString &framePointTimecode,
                                                                         qint32 clickedFirstField)
{
    // Segments: act on the segment holding the clicked frame
    const QVector<TbcMetaData::Segment> &segments = hooks_.segments();
    const qint32 segmentIndexAtFrame = clickedFirstField >= 0 ? segmentIndexContainingField(clickedFirstField) : -1;
    QAction *setInOutFromSegmentAction = nullptr;
    QAction *cutSegmentAction = nullptr;
    if (!segments.isEmpty()) {
        menu->addSeparator();
        const qint32 segmentId = segmentIndexAtFrame >= 0 ? segments.at(segmentIndexAtFrame).id : -1;
        setInOutFromSegmentAction = menu->addAction(segmentIndexAtFrame >= 0
                                                        ? tr("Set In/Out From Segment %1").arg(segmentId)
                                                        : tr("Set In/Out From Segment"));
        setInOutFromSegmentAction->setEnabled(segmentIndexAtFrame >= 0 && !hooks_.isMetadataOnly());
        cutSegmentAction = menu->addAction(tr("Cut Segment Here (%1 | %2)")
                                               .arg(framePoint)
                                               .arg(framePointTimecode));
        cutSegmentAction->setToolTip(Glossary::tooltip(Glossary::Term::Cut));
        cutSegmentAction->setEnabled(segmentIndexAtFrame >= 0
                                     && clickedFirstField > segments.at(segmentIndexAtFrame).startField);
    }
    // Shot boundaries: mark or unmark the one at the clicked frame as a scene
    // change, or add one there
    QAction *shotBoundaryAction = nullptr;
    const qint32 boundaryIndex = clickedFirstField >= 0 ? shotBoundaryNearField(clickedFirstField) : -1;
    if (hooks_.isLoaded() && clickedFirstField >= 0) {
        if (boundaryIndex >= 0) {
            const ShotBoundary &boundary = shotBoundaries_.at(boundaryIndex);
            shotBoundaryAction = menu->addAction(boundary.sceneChange
                                                     ? tr("Unmark Scene Change (shot boundary %1)").arg(boundary.id)
                                                     : tr("Mark Scene Change (shot boundary %1)").arg(boundary.id));
            shotBoundaryAction->setToolTip(Glossary::tooltip(Glossary::Term::SceneChange));
        } else {
            shotBoundaryAction = menu->addAction(tr("Add Shot Boundary Here (%1 | %2)").arg(framePoint).arg(framePointTimecode));
            shotBoundaryAction->setToolTip(Glossary::tooltip(Glossary::Term::ShotBoundary));
        }
    }
    QAction *openSegmentsViewerAction = menu->addAction(tr("Open Segments Viewer..."));
    return [this, setInOutFromSegmentAction, cutSegmentAction, openSegmentsViewerAction, segmentIndexAtFrame,
            clickedFirstField, shotBoundaryAction, boundaryIndex](QAction *selectedAction) {
        if (!selectedAction) {
            return false;
        }
        if (shotBoundaryAction && selectedAction == shotBoundaryAction) {
            QVector<ShotBoundary> boundaries = shotBoundaries_;
            const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            if (boundaryIndex >= 0 && boundaryIndex < boundaries.size()) {
                // Marking a scene change is a person's review: it confirms the boundary
                ShotBoundary &boundary = boundaries[boundaryIndex];
                boundary.sceneChange = !boundary.sceneChange;
                boundary.verdict = QStringLiteral("confirmed");
                boundary.source = QStringLiteral("user");
                boundary.updatedAt = now;
            } else {
                ShotBoundary boundary;
                qint32 maxId = 0;
                for (const ShotBoundary &existing : boundaries) maxId = qMax(maxId, existing.id);
                boundary.id = maxId + 1;
                boundary.field = clickedFirstField;
                boundary.source = QStringLiteral("user");
                boundary.verdict = QStringLiteral("confirmed");
                boundary.confidence = 1.0;
                boundary.reason = tr("added by hand");
                boundary.createdBy = QStringLiteral("tbc-analyse");
                boundary.updatedAt = now;
                boundaries.append(boundary);
                std::stable_sort(boundaries.begin(), boundaries.end(), [](const ShotBoundary &a, const ShotBoundary &b) {
                    return a.field < b.field;
                });
            }
            applyShotBoundaries(boundaries);
            return true;
        }
        if (setInOutFromSegmentAction && selectedAction == setInOutFromSegmentAction) {
            setInOutFromSegment(segmentIndexAtFrame, true, true);
            return true;
        }
        if (cutSegmentAction && selectedAction == cutSegmentAction) {
            QString cutStatus;
            cutSegmentAtField(clickedFirstField, &cutStatus);
            hooks_.showStatus(cutStatus);
            refreshViewer();
            return true;
        }
        if (selectedAction == openSegmentsViewerAction) {
            showViewer();
            return true;
        }
        return false;
    };
}
