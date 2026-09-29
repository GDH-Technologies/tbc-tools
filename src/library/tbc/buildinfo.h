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

// The build's identity: its version, how it was built, and its source.
//
// Only buildinfo.cpp is compiled with APP_VERSION, APP_BUILD and APP_SOURCE_ID,
// so changing any of them recompiles that one file and relinks, rather than
// recompiling every translation unit.
//
// The source id identifies the source tree, NOT a commit: every commit with
// identical content gets the same value (so a PR's build and its merge on main
// are the same derivation). Nix builds set APP_BUILD "nix" and APP_SOURCE_ID
// "src-<first 12 chars of the filtered source's store hash>"; a CMake build
// from a git checkout sets "git" and "tree-<first 12 chars of HEAD's tree>";
// anything else reports "unknown". The commit a host runs is in its install
// record (the Nix profile's locked flake URL), never in these strings.
namespace TbcBuildInfo {

QString version();
QString build();
QString sourceId();

// "tbc-tools <version> - Build: <build> / Source: <source id>", the string every
// CLI tool reports through QCoreApplication::setApplicationVersion(), so
// `<tool> --version` shows which release each host runs.
QString versionLine();

} // namespace TbcBuildInfo

#endif // BUILDINFO_H
