/******************************************************************************
 * vbiprocessingoptions.h
 * tbc-analyse / tbc-process-vbi
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Harry Munday
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef VBIPROCESSINGOPTIONS_H
#define VBIPROCESSINGOPTIONS_H

#include <QString>

// VBI processing options - which data types ld-process-vbi should decode, plus
// the teletext advanced options. Default-constructed options match the
// ld-process-vbi CLI defaults (four in-process VBI decoders on; teletext and
// VITS off). Defined as a free type so ld-analyse (Configuration) and the
// standalone tbc-process-vbi GUI can both use it without coupling to the
// Configuration QSettings class.
struct VbiProcessingOptions {
    bool vbiCore = true;
    bool ntsc = true;
    bool vitc = true;
    bool closedCaptions = true;
    bool teletext = false;
    bool vits = false;
    QString teletextHtmlDir;                 // empty -> ld-process-vbi default (<input>_teletext_html)
    QString teletextTapeFormat = QStringLiteral("vhs");
    qint32 teletextMinDuplicates = 1;
};

#endif // VBIPROCESSINGOPTIONS_H
