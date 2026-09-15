/************************************************************************

    buildinfo.h

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

#ifndef BUILDINFO_H
#define BUILDINFO_H

#include <QString>

// The build's identity: its version, branch and commit.
//
// Only buildinfo.cpp is compiled with APP_VERSION, APP_BRANCH and APP_COMMIT,
// so changing any of them recompiles that one file and relinks, rather than
// recompiling every translation unit. Nix builds set APP_COMMIT to
// "src-<first 12 chars of the filtered source's store hash>". That is a tree
// id: every commit with identical content gets the same value, and so the same
// derivation. They set APP_BRANCH to "nix".
namespace TbcBuildInfo {

QString version();
QString branch();
QString commit();

// "tbc-tools - Branch: <branch> / Commit: <commit>", the string every CLI tool
// reports through QCoreApplication::setApplicationVersion().
QString versionLine();

} // namespace TbcBuildInfo

#endif // BUILDINFO_H
