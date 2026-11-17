/******************************************************************************
 * testsegmentscontroller.cpp
 * tbc-segments - tbc-analyse segment behaviour (SegmentsController) tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Reece Dodge
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>
#include <QtGlobal>

#if !defined(Q_OS_WIN)
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QStandardPaths>
#include <QWidget>

#include "segmentscontroller.h"
#endif

// Release builds define NDEBUG, so a bare assert() would check nothing.
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "     \
                      << #condition << "\n";                                \
            std::exit(1);                                                   \
        }                                                                   \
    } while (false)

#if !defined(Q_OS_WIN)
namespace {

TbcMetaData::Segment segment(qint32 id, qint32 startField, qint32 endFieldExclusive, const QString &kind = contentSpanKind(),
                             const QString &source = QStringLiteral("derived"))
{
    TbcMetaData::Segment result;
    result.id = id;
    result.startField = startField;
    result.endFieldExclusive = endFieldExclusive;
    result.kind = kind;
    result.source = source;
    result.enabled = kind == contentSpanKind();
    return result;
}

// A fake host: 100 frames / 200 fields, frame n holds fields 2n-2 and 2n-1
// (0-based), slider positions are frames, no field view
struct FakeHost {
    QVector<TbcMetaData::Segment> segments;
    QVector<TbcMetaData::Segment> derived;
    QVector<ShotBoundary> detected;     // what a fresh detection finds
    qint32 currentFrame = 1;
    qint32 edits = 0;
    qint32 inPoint = -1;
    qint32 outPoint = -1;
    qint32 goneTo = -1;
    QString lastStatus;

    SegmentsController::Hooks hooks()
    {
        SegmentsController::Hooks h;
        h.isLoaded = []() { return true; };
        h.isMetadataOnly = []() { return false; };
        h.numberOfFrames = []() { return 100; };
        h.numberOfFields = []() { return 200; };
        h.segments = [this]() -> const QVector<TbcMetaData::Segment> & { return segments; };
        h.setSegments = [this](const QVector<TbcMetaData::Segment> &s) { segments = s; };
        // The real layer rule on fake data: merge the detection with the
        // boundaries in hand; the segments are the derived ones
        h.deriveLayer = [this](const SegmentsThresholds &, const QVector<ShotBoundary> &stored) {
            SegmentLayer layer;
            layer.shotBoundaries = mergeShotBoundaries(stored, detected);
            layer.segments = derived;
            return layer;
        };
        h.metadataPath = []() { return QString(); };
        h.derivedAtLoad = []() { return false; };
        h.evidenceAvailable = []() { return true; };
        h.segmentFrameRange = [](const TbcMetaData::Segment &s, qint32 *start, qint32 *length) {
            *start = s.startField / 2 + 1;
            *length = (s.endFieldExclusive - s.startField) / 2;
            return *length > 0;
        };
        h.frameContainingField = [](qint32 field) { return field / 2 + 1; };
        h.currentFrame = [this]() { return currentFrame; };
        h.currentFirstField = [this]() { return (currentFrame - 1) * 2; };
        h.sliderPositionForField = [](qint32 field) { return field / 2 + 1; };
        h.frameToTimecode = [](qint32 frame) { return QString::number(frame); };
        h.timecodeFrameRate = []() { return 30000.0 / 1001.0; };
        h.timecodeFrameBaseRate = []() { return 30; };
        h.goToField = [this](qint32 field) { goneTo = field; };
        h.showOrRaise = [](QWidget *) {};
        h.showStatus = [this](const QString &text) { lastStatus = text; };
        h.canSetInOut = []() { return true; };
        h.setInPoint = [this](qint32 frame) { inPoint = frame; };
        h.setOutPoint = [this](qint32 frame) { outPoint = frame; };
        h.segmentsEdited = [this]() { edits++; };
        return h;
    }
};

// Content span [0,60), noise [60,80), content span [80,200)
QVector<TbcMetaData::Segment> threeSegments()
{
    return {segment(1, 0, 60), segment(2, 60, 80, QStringLiteral("noise")), segment(3, 80, 200)};
}

void testLookupsAndReadout()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();
    SegmentsController controller(&window, host.hooks());
    CHECK(controller.hasSegments());
    CHECK(controller.segmentIndexContainingField(0) == 0);
    CHECK(controller.segmentIndexContainingField(65) == 1);
    CHECK(controller.segmentIndexContainingField(199) == 2);
    CHECK(controller.segmentIndexContainingField(200) == -1);
    CHECK(controller.statusReadout(1) == QStringLiteral(" Seg: 1/3"));
    CHECK(controller.statusReadout(81) == QStringLiteral(" Seg: 3/3"));
    CHECK(controller.statusReadout(400) == QStringLiteral(" Seg: -/3"));
    host.segments.clear();
    CHECK(controller.statusReadout(1).isEmpty());
}

void testChapterSkipTargets()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();     // starts at frames 1, 31, 41
    SegmentsController controller(&window, host.hooks());
    CHECK(controller.nextSegmentStartFrame(1, 100) == 31);
    CHECK(controller.nextSegmentStartFrame(31, 100) == 41);
    CHECK(controller.nextSegmentStartFrame(41, 100) == 100);
    CHECK(controller.previousSegmentStartFrame(41) == 31);
    CHECK(controller.previousSegmentStartFrame(31) == 1);
}

void testTimelineMarkers()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();
    host.segments[2].enabled = false;
    SegmentsController controller(&window, host.hooks());
    QVector<qint32> positions;
    QStringList tooltips;
    QVector<SegmentTimelineSpan> spans;
    controller.timelineMarkers(&positions, &tooltips, &spans);
    // A tick at every boundary but the first
    CHECK(positions == QVector<qint32>({31, 41}));
    CHECK(tooltips.size() == 2);
    CHECK(tooltips.at(1).contains(QStringLiteral("[disabled]")));
    // Tints: the noise span and the disabled content span
    CHECK(spans.size() == 2);
    CHECK(spans.at(0).startPosition == 31 && spans.at(0).endPosition == 40);
    CHECK(spans.at(1).startPosition == 41 && spans.at(1).endPosition == 100);
}

void testCutAndSetInOut()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();
    SegmentsController controller(&window, host.hooks());
    QString status;
    CHECK(!controller.cutSegmentAtField(0, &status));            // a segment start is no cut
    CHECK(controller.cutSegmentAtField(100, &status));
    CHECK(host.edits == 1);
    CHECK(host.segments.size() == 4);
    CHECK(host.segments.at(2).endFieldExclusive == 100 && host.segments.at(2).source == QLatin1String("user"));
    CHECK(host.segments.at(3).id == 4 && host.segments.at(3).startField == 100
          && host.segments.at(3).createdBy == QLatin1String("tbc-analyse"));

    host.currentFrame = 35;                                       // field 68: the noise segment
    controller.setInOutFromSegmentAtCurrentFrame(true, true);
    CHECK(host.inPoint == 31 && host.outPoint == 40);
}

// Re-derive keeps every user segment and drops the derived ones it overlaps
void testRederiveKeepsUserSegments()
{
    QWidget window;
    FakeHost host;
    host.segments = {segment(1, 0, 60), segment(7, 60, 120, contentSpanKind(), QStringLiteral("user")), segment(3, 120, 200)};
    host.derived = {segment(1, 0, 50), segment(2, 50, 100), segment(3, 100, 200)};
    SegmentsController controller(&window, host.hooks());
    controller.rederiveSegments(SegmentsThresholds());
    CHECK(host.edits == 1);
    // The user segment stays; derived [0,50) stays; [50,100) and [100,200) overlap it and go
    CHECK(host.segments.size() == 2);
    CHECK(host.segments.at(0).startField == 0 && host.segments.at(0).endFieldExclusive == 50 && host.segments.at(0).id == 8);
    CHECK(host.segments.at(1).id == 7 && host.segments.at(1).source == QLatin1String("user"));
    CHECK(host.lastStatus.contains(QStringLiteral("1 derived (2 dropped for user segments), 1 user kept")));
}

void testTimelineMenu()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();
    SegmentsController controller(&window, host.hooks());
    QMenu menu;
    const auto handle = controller.addTimelineMenuActions(&menu, 50, QStringLiteral("50"), 98);
    QAction *setInOut = nullptr;
    QAction *cut = nullptr;
    QAction *openViewer = nullptr;
    for (QAction *action : menu.actions()) {
        if (action->text().startsWith(QStringLiteral("Set In/Out From Segment"))) setInOut = action;
        if (action->text().startsWith(QStringLiteral("Cut Segment Here"))) cut = action;
        if (action->text() == QStringLiteral("Open Segments Viewer...")) openViewer = action;
    }
    CHECK(setInOut && cut && openViewer);
    CHECK(setInOut->text() == QStringLiteral("Set In/Out From Segment 3"));
    CHECK(cut->isEnabled());
    QAction unrelated(QStringLiteral("other"));
    CHECK(!handle(&unrelated));
    CHECK(!handle(nullptr));
    CHECK(handle(cut));
    CHECK(host.segments.size() == 4 && host.segments.at(3).startField == 98);

    // Without segments: no segment items, only adding a shot boundary and the viewer
    host.segments.clear();
    QMenu empty;
    controller.addTimelineMenuActions(&empty, 1, QStringLiteral("1"), 0);
    CHECK(empty.actions().size() == 2);
    CHECK(empty.actions().at(0)->text().startsWith(QStringLiteral("Add Shot Boundary Here")));
    CHECK(empty.actions().at(1)->text() == QStringLiteral("Open Segments Viewer..."));
}

ShotBoundary boundaryAt(qint32 id, qint32 field)
{
    ShotBoundary boundary;
    boundary.id = id;
    boundary.field = field;
    boundary.confidence = 0.9;
    boundary.reason = QStringLiteral("matched field pair");
    return boundary;
}

// Shot boundaries: detected at load when none are stored; a tick and a
// chapter-skip stop each unless rejected; they never change the segments
void testShotBoundariesAtLoad()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();
    host.derived = threeSegments();
    host.detected = {boundaryAt(1, 20), boundaryAt(2, 120)};
    SegmentsController controller(&window, host.hooks());
    controller.sourceLoaded();
    CHECK(controller.shotBoundaries().size() == 2 && controller.shotBoundaries().at(0).verdict == QLatin1String("detected"));
    CHECK(host.edits == 0 && host.segments.size() == 3);
    QVector<qint32> positions;
    QStringList tooltips;
    controller.shotBoundaryTicks(&positions, &tooltips);
    CHECK(positions == QVector<qint32>({11, 61}));
    CHECK(tooltips.at(0).contains(QStringLiteral("matched field pair")));
    CHECK(controller.nextSegmentStartFrame(1, 100) == 11);
    CHECK(controller.nextSegmentStartFrame(41, 100) == 61);
    CHECK(controller.previousSegmentStartFrame(61) == 41);
    controller.sourceUnloaded();
    CHECK(controller.shotBoundaries().isEmpty());
}

// Marking a scene change from the timeline confirms the boundary and makes
// it the user's; the segments stay as they are (scenes are worked out); a
// rejected boundary loses its tick; elsewhere the menu adds a boundary
void testSceneChangeFromMenu()
{
    QWidget window;
    FakeHost host;
    host.segments = threeSegments();
    host.derived = threeSegments();
    host.detected = {boundaryAt(1, 20), boundaryAt(2, 120)};
    SegmentsController controller(&window, host.hooks());
    controller.sourceLoaded();

    QMenu menu;
    const auto handle = controller.addTimelineMenuActions(&menu, 11, QStringLiteral("11"), 20);
    QAction *markScene = nullptr;
    for (QAction *action : menu.actions()) {
        if (action->text().startsWith(QStringLiteral("Mark Scene Change"))) markScene = action;
    }
    CHECK(markScene != nullptr);
    CHECK(handle(markScene));
    const ShotBoundary &first = controller.shotBoundaries().at(0);
    CHECK(first.sceneChange && first.source == QLatin1String("user") && first.verdict == QLatin1String("confirmed"));
    CHECK(host.edits == 1 && host.segments.size() == 3);

    QVector<ShotBoundary> edited = controller.shotBoundaries();
    edited[1].verdict = QStringLiteral("rejected");
    controller.applyShotBoundaries(edited);
    QVector<qint32> positions;
    QStringList tooltips;
    controller.shotBoundaryTicks(&positions, &tooltips);
    CHECK(positions == QVector<qint32>({11}));
    CHECK(tooltips.at(0).contains(QStringLiteral("scene change")));

    QMenu other;
    controller.addTimelineMenuActions(&other, 90, QStringLiteral("90"), 178);
    bool offersAdd = false;
    for (QAction *action : other.actions()) {
        if (action->text().startsWith(QStringLiteral("Add Shot Boundary Here"))) offersAdd = true;
    }
    CHECK(offersAdd);
}

} // namespace
#endif

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    // The controller's viewer needs a QApplication, which needs a platform
    // plugin: offscreen, set by CMake. Not on Windows: there the self-hosted
    // runner's ctest hung in the QApplication constructor (testsegmentsviewer).
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    std::cout << "testsegmentscontroller: skipped on Windows\n";
    return 0;
#else
    QApplication app(argc, argv);
    // The preferences live in tbc-analyse's ini: keep the test's own
    QStandardPaths::setTestModeEnabled(true);
    testLookupsAndReadout();
    testChapterSkipTargets();
    testTimelineMarkers();
    testCutAndSetInOut();
    testRederiveKeepsUserSegments();
    testTimelineMenu();
    testShotBoundariesAtLoad();
    testSceneChangeFromMenu();
    std::cout << "testsegmentscontroller: all tests passed\n";
    return 0;
#endif
}
