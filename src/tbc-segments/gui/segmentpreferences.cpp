/************************************************************************

    segmentpreferences.cpp

    tbc-segments - tbc-analyse's segment preferences
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

#include "segmentpreferences.h"

#include <QSettings>
#include <QStandardPaths>
#include <QString>

namespace {

QString iniPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/tbc-analyse.ini");
}

bool readFlag(const char *key)
{
    QSettings settings(iniPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("segments/") + QLatin1String(key), false).toBool();
}

void writeFlag(const char *key, bool value)
{
    QSettings settings(iniPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("segments/") + QLatin1String(key), value);
    settings.sync();
}

} // namespace

namespace SegmentPreferences {

bool splitAtRestarts() { return readFlag("splitAtRestarts"); }
void setSplitAtRestarts(bool enabled) { writeFlag("splitAtRestarts", enabled); }
bool restartChapters() { return readFlag("restartChapters"); }
void setRestartChapters(bool enabled) { writeFlag("restartChapters", enabled); }

} // namespace SegmentPreferences
