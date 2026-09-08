#!/usr/bin/env python3
# gdh_version.py
#
# Resolve, propose and cut the GDH fork version for tbc-tools.
#
# This fork carries GDH-only work on top of harrypm/tbc-tools, and needs to say
# so without colonising upstream's version namespace. The scheme is:
#
#     tag       v<UPSTREAM>-gdh-<MAJOR>.<MINOR>          e.g. v3.2.8-gdh-1.0
#     version   <UPSTREAM>-gdh-<MAJOR>.<MINOR>           exactly on the tag, clean tree
#               <UPSTREAM>-gdh-<MAJOR>.<MINOR>+<N>.g<sha>[.dirty]   ahead of the tag
#
# <UPSTREAM> is upstream's OWN declared version -- the "version" field of
# vcpkg.json. That field stays correct on its own: harrypm's release workflow
# bumps it and we merge that in, while the GDH path never writes it (GDH tags
# are excluded from release.yml's trigger, so its version stamper never runs).
#
# The git tag is the only INPUT: nothing here decides a version from a file.
# `bump` does write one -- .gdh-version, holding the exact version the new tag
# names -- and commits it before tagging that same commit, so the file and the
# tag agree by construction. The file exists for consumers that cannot see
# tags: a Nix build gets only the tracked tree (flakes expose rev/revCount but
# never tags, and .git is filtered out of the flake source), so without it such
# a build silently reports upstream's version instead of the fork's.
#
# Usage:
#   scripts/gdh_version.py show                 # resolved version string
#   scripts/gdh_version.py show --json          # the full resolution, as JSON
#   scripts/gdh_version.py propose              # scan commits, print next bump
#   scripts/gdh_version.py propose --level minor
#   scripts/gdh_version.py bump --level auto --push --branch main
#
# Exit codes: 0 on success; 2 when `propose --level auto` finds nothing that
# warrants a release; other non-zero on error.
#
# NOTE for CMake consumers: CMakeLists.txt derives APP_VERSION with the same
# rule at configure time, and reads .gdh-version when there is no .git.
# Creating a tag after configuring leaves the built binary reporting the older
# version until you re-configure. CI configures fresh, so this only bites local
# incremental builds.

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The committed cache of the tag, read by CMakeLists.txt when there is no .git.
VERSION_FILE = REPO_ROOT / ".gdh-version"

# v3.2.8-gdh-1.0-38-g70803adc  ->  upstream / major / minor / distance / node
DESCRIBE_RE = re.compile(
    r"^v(?P<upstream>\d+(?:\.\d+)*)-gdh-(?P<major>\d+)\.(?P<minor>\d+)"
    r"-(?P<distance>\d+)-g(?P<node>[0-9a-f]+)$"
)

# Conventional Commits. A `!` before the colon, or a BREAKING CHANGE trailer,
# means major; a plain `feat` means minor; everything else means no release.
BREAKING_SUBJECT_RE = re.compile(r"^[a-zA-Z]+(?:\([^)]*\))?!:")
FEAT_SUBJECT_RE = re.compile(r"^feat(?:\([^)]*\))?:")
BREAKING_BODY_RE = re.compile(r"^BREAKING[ -]CHANGE:", re.MULTILINE)

# `git describe` grows its default abbreviation as a repo gets bigger, so pin it
# rather than letting the sha length drift between machines and over time.
ABBREV = "8"


class VersionError(RuntimeError):
    """A resolution or bump failure with a message fit for stderr."""


def git(*args: str, check: bool = True) -> str:
    """Run git in the repo root and return stdout, stripped."""
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args],
        capture_output=True,
        text=True,
    )
    if check and result.returncode != 0:
        raise VersionError(
            f"git {' '.join(args)} failed ({result.returncode}): {result.stderr.strip()}"
        )
    return result.stdout.strip() if result.returncode == 0 else ""


def upstream_version() -> str:
    """Upstream's declared version: the "version" field of vcpkg.json."""
    vcpkg_path = REPO_ROOT / "vcpkg.json"
    try:
        data = json.loads(vcpkg_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise VersionError(f"cannot read the upstream version from {vcpkg_path}: {exc}")
    version = data.get("version")
    if not isinstance(version, str) or not version:
        raise VersionError(f'{vcpkg_path} has no usable "version" field')
    return version


def is_dirty() -> bool:
    """Whether the working tree has uncommitted changes.

    Checked separately rather than via `git describe --dirty`, because git
    refuses to combine `--dirty` with a commit-ish and keeping the two apart
    means the describe calls below can name an explicit revision if they ever
    need to.
    """
    return bool(git("status", "--porcelain"))


def describe_gdh() -> str | None:
    """The `git describe` output for the nearest gdh tag, or None if there is none.

    Fails cleanly before the first gdh tag exists: git exits 128 with
    "No names found, cannot describe anything."
    """
    out = git(
        "describe", "--tags", "--long", "--abbrev=" + ABBREV, "--match", "v*-gdh-*",
        check=False,
    )
    return out or None


def distance_from_upstream_anchor(upstream: str) -> tuple[int, str]:
    """Commits since the upstream release tag, for the pre-first-gdh-tag case.

    `git describe --match 'v*-gdh-*'` cannot supply a distance until a gdh tag
    exists, so count from the tag naming the upstream base instead. Counting
    from the repo root would be wrong -- that yields ~1800 here.
    """
    node = "g" + git("rev-parse", f"--short={ABBREV}", "HEAD")
    anchor = f"v{upstream}"
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{anchor}", check=False):
        return int(git("rev-list", "--count", f"{anchor}..HEAD")), node
    return int(git("rev-list", "--count", "HEAD")), node


def resolve() -> dict:
    """Resolve the current version. See the module docstring for the scheme."""
    upstream = upstream_version()
    dirty = is_dirty()
    described = describe_gdh()

    upstream_moved = False
    if described:
        match = DESCRIBE_RE.match(described)
        if not match:
            raise VersionError(
                f"cannot parse gdh tag from `git describe` output {described!r}; "
                "expected v<upstream>-gdh-<major>.<minor>-<n>-g<sha>"
            )
        tagged_upstream = match.group("upstream")
        if tagged_upstream == upstream:
            major, minor = int(match.group("major")), int(match.group("minor"))
            distance, node = int(match.group("distance")), "g" + match.group("node")
        else:
            # Upstream moved since the last gdh tag. The counters reset, so no
            # gdh release exists yet on this base: report gdh-0.0 and let
            # `propose` offer gdh-1.0.
            upstream_moved = True
            major, minor = 0, 0
            distance, node = distance_from_upstream_anchor(upstream)
    else:
        major, minor = 0, 0
        distance, node = distance_from_upstream_anchor(upstream)

    # gdh-0.0 is a sentinel, never a release, so it always carries its distance.
    exact = distance == 0 and not dirty and (major, minor) != (0, 0)

    version = f"{upstream}-gdh-{major}.{minor}"
    if not exact:
        version += f"+{distance}.{node}"
        if dirty:
            version += ".dirty"

    return {
        "version": version,
        "upstream": upstream,
        "major": major,
        "minor": minor,
        "distance": distance,
        "node": node,
        "dirty": dirty,
        "exact": exact,
        "upstream_moved": upstream_moved,
        "described": described,
    }


def scan_range(state: dict) -> str:
    """The `git log` range whose commits decide the next bump."""
    if state["described"] and not state["upstream_moved"]:
        tag = f"v{state['upstream']}-gdh-{state['major']}.{state['minor']}"
        return f"{tag}..HEAD"
    anchor = f"v{state['upstream']}"
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{anchor}", check=False):
        return f"{anchor}..HEAD"
    return "HEAD"


def classify(commit_range: str) -> tuple[str, list[str]]:
    """Scan Conventional Commits in the range; return (level, reasons).

    Merge commits are skipped: their subjects are "Merge pull request #N from
    ..." and carry no type, while the commits they bring in do.
    """
    raw = git("log", "--no-merges", "--format=%H%x00%s%x00%b%x1e", commit_range)
    level, reasons = "none", []

    for record in raw.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        sha, subject, body = (record.split("\x00", 2) + ["", ""])[:3]
        short = sha[:8]

        if BREAKING_SUBJECT_RE.match(subject):
            level = "major"
            reasons.append(f"{short} MAJOR (breaking `!:`)  {subject}")
        elif BREAKING_BODY_RE.search(body):
            level = "major"
            reasons.append(f"{short} MAJOR (BREAKING CHANGE trailer)  {subject}")
        elif FEAT_SUBJECT_RE.match(subject):
            if level != "major":
                level = "minor"
            reasons.append(f"{short} minor (feat)  {subject}")

    return level, reasons


def next_tag(state: dict, level: str) -> str:
    """The tag a bump at `level` would create."""
    if state["upstream_moved"] or (state["major"], state["minor"]) == (0, 0):
        # A new upstream base restarts the GDH series.
        return f"v{state['upstream']}-gdh-1.0"
    if level == "major":
        return f"v{state['upstream']}-gdh-{state['major'] + 1}.0"
    return f"v{state['upstream']}-gdh-{state['major']}.{state['minor'] + 1}"


def do_show(args: argparse.Namespace) -> int:
    state = resolve()
    print(json.dumps(state, indent=2) if args.json else state["version"])
    return 0


def do_propose(args: argparse.Namespace) -> int:
    state = resolve()
    commit_range = scan_range(state)
    scanned, reasons = classify(commit_range)
    level = scanned if args.level == "auto" else args.level

    print(f"upstream base    {state['upstream']}  (vcpkg.json)")
    print(f"current version  {state['version']}")
    print(f"scanned range    {commit_range}")
    if state["upstream_moved"]:
        print(
            f"note             upstream moved to {state['upstream']} since the last gdh "
            "tag; the GDH counters reset to 1.0"
        )
    print()

    if reasons:
        print("commits driving the level:")
        for reason in reasons:
            print(f"  {reason}")
    else:
        print("no feat/breaking commits in range")
    print()

    if level == "none":
        print("proposed         no bump -- nothing in range warrants a release")
        print("                 (override with --level minor or --level major)")
        return 2

    proposed = next_tag(state, level)
    source = "scanned" if args.level == "auto" else "forced"
    print(f"proposed level   {level}  ({source})")
    print(f"proposed tag     {proposed}")
    print(f"proposed version {proposed.lstrip('v')}")
    return 0


def do_bump(args: argparse.Namespace) -> int:
    state = resolve()
    if state["dirty"]:
        raise VersionError("refusing to tag a dirty working tree; commit or clean it first")

    commit_range = scan_range(state)
    scanned, _ = classify(commit_range)
    level = scanned if args.level == "auto" else args.level
    if level == "none":
        raise VersionError(
            f"nothing in {commit_range} warrants a release; "
            "pass --level minor or --level major to force one"
        )

    tag = next_tag(state, level)
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}", check=False):
        raise VersionError(f"tag {tag} already exists")

    # Commit the cache of the tag BEFORE tagging, so the tag lands on the commit
    # that carries the file: `git describe` still reports distance 0, and a
    # build from the tag with no .git reads the same version from the file.
    version = tag.lstrip("v")
    VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    if git("status", "--porcelain", "--", str(VERSION_FILE)):
        git("add", "--", str(VERSION_FILE))
        git("commit", "-q", "-m", f"chore(version): {version}")
        print(f"committed {VERSION_FILE.name} = {version}")

    git("tag", "-a", tag, "-m", f"GDH release {version}")
    print(f"created {tag}")

    if args.push:
        # The version commit has to reach the branch before the tag does, or the
        # tag names a commit that is on no branch.
        branch = args.branch or git("symbolic-ref", "--short", "HEAD", check=False)
        if not branch:
            raise VersionError(
                "HEAD is detached; pass --branch to say where the version commit goes"
            )
        git("push", "origin", f"HEAD:refs/heads/{branch}")
        git("push", "origin", tag)
        print(f"pushed {branch} and {tag} to origin")
    else:
        print(
            "not pushed; run: git push origin HEAD:refs/heads/<branch> "
            f"&& git push origin {tag}"
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    show = sub.add_parser("show", help="print the resolved version")
    show.add_argument("--json", action="store_true", help="print the full resolution")
    show.set_defaults(func=do_show)

    propose = sub.add_parser("propose", help="scan commits and print the next bump")
    propose.add_argument("--level", choices=("auto", "major", "minor"), default="auto")
    propose.set_defaults(func=do_propose)

    bump = sub.add_parser("bump", help="create the next gdh tag")
    bump.add_argument("--level", choices=("auto", "major", "minor"), default="auto")
    bump.add_argument("--push", action="store_true",
                      help="push the version commit and the tag to origin")
    bump.add_argument("--branch", default=None,
                      help="branch the version commit belongs on "
                           "(required with --push when HEAD is detached, as in CI)")
    bump.set_defaults(func=do_bump)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except VersionError as exc:
        print(f"gdh_version: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
