/************************************************************************

    buildinfo.cpp

    tbc-tools TBC library
    Copyright (C) 2026 GDH-Technologies LLC

    This file is part of tbc-tools.

    tbc-tools is free software: you can redistribute it and/or
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

#include "tbc/buildinfo.h"

// src/library/CMakeLists.txt gives these to this file alone.
#ifndef APP_VERSION
#error "APP_VERSION is not defined for buildinfo.cpp; see src/library/CMakeLists.txt"
#endif
#ifndef APP_BRANCH
#error "APP_BRANCH is not defined for buildinfo.cpp; see src/library/CMakeLists.txt"
#endif
#ifndef APP_COMMIT
#error "APP_COMMIT is not defined for buildinfo.cpp; see src/library/CMakeLists.txt"
#endif

namespace TbcBuildInfo {

QString version()
{
    return QStringLiteral(APP_VERSION);
}

QString branch()
{
    return QStringLiteral(APP_BRANCH);
}

QString commit()
{
    return QStringLiteral(APP_COMMIT);
}

QString versionLine()
{
    return QStringLiteral("tbc-tools - Branch: %1 / Commit: %2").arg(branch(), commit());
}

} // namespace TbcBuildInfo
