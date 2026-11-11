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
    result.kind = QStringLiteral("clip");
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

// Move Start, Move End and Split are enabled exactly where the move is valid,
// so a click never has to be refused
void testMoveAndSplitEnabled()
{
    std::cerr << "Testing Move Start/End and Split enablement\n";

    SegmentsViewerDialog dialog;
    QTableWidget *table = dialog.findChild<QTableWidget *>();
    CHECK(table != nullptr);
    QPushButton *moveStart = button(dialog, QStringLiteral("Move Start Here"));
    QPushButton *moveEnd = button(dialog, QStringLiteral("Move End Here"));
    QPushButton *split = button(dialog, QStringLiteral("Split At Current Frame"));

    // Current field 20: inside the first segment
    setState(dialog, 20);
    table->clearSelection();
    CHECK(!moveStart->isEnabled() && !moveEnd->isEnabled() && !split->isEnabled());

    table->selectRow(0);
    CHECK(moveStart->isEnabled());
    CHECK(moveEnd->isEnabled());
    CHECK(split->isEnabled());

    // The second segment: its start can move back to 20 (after the first
    // segment's start), but its end can't move before its own start, and 20
    // isn't inside it to split
    table->selectRow(1);
    CHECK(moveStart->isEnabled());
    CHECK(!moveEnd->isEnabled());
    CHECK(!split->isEnabled());

    // Current field 0: the second segment's start can't reach the first's
    setState(dialog, 0);
    table->selectRow(1);
    CHECK(!moveStart->isEnabled());

    // No current frame: nothing to move to or split at
    setState(dialog, -1);
    table->selectRow(0);
    CHECK(!moveStart->isEnabled() && !moveEnd->isEnabled() && !split->isEnabled());
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
    testMoveAndSplitEnabled();
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif

    std::cerr << "All segments viewer tests passed\n";
    return 0;
}
