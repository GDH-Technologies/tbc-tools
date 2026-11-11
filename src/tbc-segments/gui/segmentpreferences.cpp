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

namespace {

QString iniPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/tbc-analyse.ini");
}

} // namespace

namespace SegmentPreferences {

QString chaptersBy()
{
    QSettings settings(iniPath(), QSettings::IniFormat);
    const QString value = settings.value(QStringLiteral("segments/chaptersBy"), QStringLiteral("segment")).toString();
    return (value == QLatin1String("scene") || value == QLatin1String("shot")) ? value : QStringLiteral("segment");
}

void setChaptersBy(const QString &chaptersBy)
{
    QSettings settings(iniPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("segments/chaptersBy"), chaptersBy);
    settings.sync();
}

} // namespace SegmentPreferences
