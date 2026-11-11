/******************************************************************************
 * testsegmentsviewer.cpp
 * tbc-analyse - Segments viewer unit tests
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
#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QTableWidget>

#include "segmentsviewerdialog.h"
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

QPushButton *button(const SegmentsViewerDialog &dialog, const QString &text)
{
    for (QPushButton *candidate : dialog.findChildren<QPushButton *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    std::cerr << "FAIL no button " << text.toStdString() << "\n";
    std::exit(1);
}

TbcMetaData::Segment segment(qint32 id, qint32 startField, qint32 endFieldExclusive)
{
    TbcMetaData::Segment result;
    result.id = id;
    result.startField = startField;
    result.endFieldExclusive = endFieldExclusive;
    result.kind = contentSpanKind();
    return result;
}

// Two segments, fields [0, 50) and [50, 100), with the current frame's first
// field at currentFirstField (-1: no current frame)
void setState(SegmentsViewerDialog &dialog, qint32 currentFirstField)
{
    SegmentsViewerState state;
    state.totalFrames = 50;
    state.totalFields = 100;
    state.currentFrame = currentFirstField < 0 ? -1 : currentFirstField / 2 + 1;
    state.currentFirstField = currentFirstField;
    for (const TbcMetaData::Segment &each : {segment(1, 0, 50), segment(2, 50, 100)}) {
        SegmentsViewerRow row;
        row.segment = each;
        row.hasFrames = true;
        row.startFrame = each.startField / 2 + 1;
        row.lengthFrames = (each.endFieldExclusive - each.startField) / 2;
        state.rows.append(row);
    }
    dialog.setState(state);
}

// Move Start, Move End and Cut are enabled exactly where the move is valid,
// so a click never has to be refused
void testMoveAndCutEnabled()
{
    std::cerr << "Testing Move Start/End and Cut enablement\n";

    SegmentsViewerDialog dialog;
    QTableWidget *table = dialog.findChild<QTableWidget *>(QStringLiteral("segmentsTable"));
    CHECK(table != nullptr);
    QPushButton *moveStart = button(dialog, QStringLiteral("Move Start Here"));
    QPushButton *moveEnd = button(dialog, QStringLiteral("Move End Here"));
    QPushButton *cut = button(dialog, QStringLiteral("Cut At Current Frame"));

    // Current field 20: inside the first segment
    setState(dialog, 20);
    table->clearSelection();
    CHECK(!moveStart->isEnabled() && !moveEnd->isEnabled() && !cut->isEnabled());

    table->selectRow(0);
    CHECK(moveStart->isEnabled());
    CHECK(moveEnd->isEnabled());
    CHECK(cut->isEnabled());

    // The second segment: its start can move back to 20 (after the first
    // segment's start), but its end can't move before its own start, and 20
    // isn't inside it to cut
    table->selectRow(1);
    CHECK(moveStart->isEnabled());
    CHECK(!moveEnd->isEnabled());
    CHECK(!cut->isEnabled());

    // Current field 0: the second segment's start can't reach the first's
    setState(dialog, 0);
    table->selectRow(1);
    CHECK(!moveStart->isEnabled());

    // No current frame: nothing to move to or cut at
    setState(dialog, -1);
    table->selectRow(0);
    CHECK(!moveStart->isEnabled() && !moveEnd->isEnabled() && !cut->isEnabled());
}

// The Shot Boundaries tab: ticking Scene Change confirms the boundary and
// makes it the user's; a verdict, the scene's title and enabled flag and the
// shot's flag wait for Apply; Apply hands the boundaries over; Add makes a
// confirmed user boundary at the current frame
void testShotBoundariesTab()
{
    std::cerr << "Testing the Shot Boundaries tab\n";
    SegmentsViewerDialog dialog;
    QTableWidget *table = dialog.findChild<QTableWidget *>(QStringLiteral("shotBoundaryTable"));
    CHECK(table != nullptr);
    SegmentsViewerState state;
    state.totalFrames = 50;
    state.totalFields = 100;
    state.currentFrame = 11;
    state.currentFirstField = 20;
    ShotBoundary a;
    a.id = 1; a.field = 30; a.confidence = 0.92; a.reason = QStringLiteral("matched field pair");
    ShotBoundary b = a;
    b.id = 2; b.field = 70;
    state.shotBoundaries = {a, b};
    state.shotBoundaryFrames = {16, 36};
    dialog.setState(state);
    CHECK(table->rowCount() == 2);
    CHECK(table->item(0, 4)->text() == QStringLiteral("92%"));
    CHECK(table->item(1, 2)->text() == QStringLiteral("36"));

    QVector<ShotBoundary> applied;
    QObject::connect(&dialog, &SegmentsViewerDialog::shotBoundariesUpdated,
                     [&applied](const QVector<ShotBoundary> &updated) { applied = updated; });

    table->item(0, 8)->setCheckState(Qt::Checked);        // Scene Change
    table->item(0, 9)->setText(QStringLiteral("Wedding ceremony"));
    table->item(1, 11)->setCheckState(Qt::Unchecked);     // Shot Enabled
    auto *verdict = qobject_cast<QComboBox *>(table->cellWidget(1, 7));
    CHECK(verdict != nullptr);
    verdict->setCurrentText(QStringLiteral("rejected"));
    QPushButton *apply = nullptr;
    for (QPushButton *candidate : dialog.findChildren<QPushButton *>()) {
        if (candidate->text().remove(QLatin1Char('&')) == QStringLiteral("Apply")) apply = candidate;
    }
    CHECK(apply && apply->isEnabled() && applied.isEmpty());
    apply->click();
    CHECK(applied.size() == 2);
    CHECK(applied[0].sceneChange && applied[0].sceneTitle == QLatin1String("Wedding ceremony"));
    CHECK(applied[0].source == QLatin1String("user") && applied[0].verdict == QLatin1String("confirmed"));
    CHECK(!applied[1].shotEnabled && applied[1].verdict == QLatin1String("rejected") && applied[1].field == 70);

    QPushButton *add = button(dialog, QStringLiteral("Add Shot Boundary At Current Frame"));
    CHECK(add->isEnabled());
    add->click();
    CHECK(table->rowCount() == 3 && table->item(0, 6)->text() == QStringLiteral("user"));
}

} // namespace
#endif

int main(int argc, char *argv[])
{
    // The dialog needs a QApplication, which needs a platform plugin:
    // offscreen, set by CMake. Not on Windows: there the self-hosted runner's
    // ctest (a service session, before windeployqt stages the plugins) hung in
    // the QApplication constructor.
#if !defined(Q_OS_WIN)
    QApplication application(argc, argv);
    testMoveAndCutEnabled();
    testShotBoundariesTab();
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif

    std::cerr << "All segments viewer tests passed\n";
    return 0;
}
