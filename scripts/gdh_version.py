#!/usr/bin/env python3
# gdh_version.py
#
# Resolve, propose, bump, tag and check the GDH fork version for tbc-tools.
#
# This fork carries GDH-only work on top of harrypm/tbc-tools, and needs to say
# so without colonising upstream's version namespace. The scheme is:
#
#     version   <UPSTREAM>-gdh-<MAJOR>.<MINOR>           exactly on the release commit, clean tree
#               <UPSTREAM>-gdh-<MAJOR>.<MINOR>+<N>.g<sha>[.dirty]   N commits after it
#     tag       v<UPSTREAM>-gdh-<MAJOR>.<MINOR>          e.g. v3.2.9-gdh-2.1
#
# <UPSTREAM> is upstream's OWN declared version -- the "version" field of
# vcpkg.json. That field stays correct on its own: harrypm's release workflow
# bumps it and we merge that in, while the GDH path never writes it (GDH tags
# are excluded from release.yml's trigger, so its version stamper never runs).
#
# A release is a committed .gdh-version. It rides inside a normal PR: `bump`
# writes and commits it on the branch, and the PR's own builds already carry the
# new version. A committed .gdh-version that names a NEWER version than the
# nearest gdh tag is the version: exact on the commit that introduced it on the
# first-parent line (on main, the PR's merge commit), with +N.g<sha> after it.
# The tag is then a record, not an input: gdh-version-tag.yml runs `tag` on every
# push to main and puts v<version> on that same commit. It has to be that way
# round, because the merge button creates the commit that lands on main, so no
# tag can name it before the merge. Once the tag exists it and the file agree,
# and `git describe` gives the same answer as the file did.
#
# The file also serves consumers that cannot see git at all: a Nix build gets
# only the tracked tree (.git is filtered out of the flake source), and reads it
# directly.
#
# Usage:
#   scripts/gdh_version.py show                    # resolved version string
#   scripts/gdh_version.py show --json             # the full resolution, as JSON
#   scripts/gdh_version.py propose                 # scan commits, print next bump
#   scripts/gdh_version.py propose --level minor
#   scripts/gdh_version.py bump --level minor      # commit .gdh-version on this branch
#   scripts/gdh_version.py tag --push              # tag the release commit (CI, on main)
#   scripts/gdh_version.py check-pr --base origin/main   # the PR guardrail
#
# Exit codes: 0 on success; 2 when `propose --level auto` finds nothing that
# warrants a release; 1 on an error or a failed check.
#
# NOTE for CMake consumers: CMakeLists.txt derives APP_VERSION with the same
# rule at configure time, and ci/tests/test_gdh_version.py holds the two in
# agreement. Change one, change both.

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The release version. Read by CMakeLists.txt and flake.nix as well as here.
VERSION_FILE = REPO_ROOT / ".gdh-version"
VERSION_FILE_NAME = ".gdh-version"

# v3.2.8-gdh-1.0-38-g70803adc  ->  upstream / major / minor / distance / node
DESCRIBE_RE = re.compile(
    r"^v(?P<upstream>\d+(?:\.\d+)*)-gdh-(?P<major>\d+)\.(?P<minor>\d+)"
    r"-(?P<distance>\d+)-g(?P<node>[0-9a-f]+)$"
)
# The content of .gdh-version: 3.2.9-gdh-2.1
VERSION_RE = re.compile(r"^(?P<upstream>\d+(?:\.\d+)*)-gdh-(?P<major>\d+)\.(?P<minor>\d+)$")

# Conventional Commits. A `!` before the colon, or a BREAKING CHANGE trailer,
# means major; a plain `feat` means minor; everything else means no release.
BREAKING_SUBJECT_RE = re.compile(r"^[a-zA-Z]+(?:\([^)]*\))?!:")
FEAT_SUBJECT_RE = re.compile(r"^feat(?:\([^)]*\))?:")
BREAKING_BODY_RE = re.compile(r"^BREAKING[ -]CHANGE:", re.MULTILINE)

# `git describe` grows its default abbreviation as a repo gets bigger, so pin it
# rather than letting the sha length drift between machines and over time.
ABBREV = "8"


class VersionError(RuntimeError):
    """A resolution, bump or check failure with a message fit for stderr."""


def git(*args: str, check: bool = True, raw: bool = False) -> str | None:
    """Run git in the repo root and return stdout.

    Stripped by default. With raw=True the bytes are decoded untouched -- no
    newline translation, no stripping -- and a failed command returns None,
    so a missing file and an empty one can be told apart.
    """
    result = subprocess.run(["git", "-C", str(REPO_ROOT), *args], capture_output=True)
    if check and result.returncode != 0:
        raise VersionError(
            f"git {' '.join(args)} failed ({result.returncode}): "
            f"{result.stderr.decode('utf-8', 'replace').strip()}"
        )
    if raw:
        return result.stdout.decode("utf-8", "replace") if result.returncode == 0 else None
    return result.stdout.decode("utf-8", "replace").strip() if result.returncode == 0 else ""


def is_ancestor(commit: str, of: str) -> bool:
    """Whether `commit` is reachable from `of`."""
    return subprocess.run(
        ["git", "-C", str(REPO_ROOT), "merge-base", "--is-ancestor", commit, of],
        capture_output=True,
    ).returncode == 0


def upstream_version(rev: str | None = None) -> str:
    """Upstream's declared version: the "version" field of vcpkg.json.

    From the working tree by default (as CMake reads it), or as committed at
    `rev`.
    """
    vcpkg_path = REPO_ROOT / "vcpkg.json"
    try:
        if rev is None:
            text = vcpkg_path.read_text(encoding="utf-8")
        else:
            text = git("show", f"{rev}:vcpkg.json", raw=True)
            if text is None:
                raise OSError(f"no vcpkg.json at {rev}")
        data = json.loads(text)
    except (OSError, json.JSONDecodeError) as exc:
        raise VersionError(f"cannot read the upstream version from {vcpkg_path}: {exc}")
    version = data.get("version")
    if not isinstance(version, str) or not version:
        raise VersionError(f'{vcpkg_path} has no usable "version" field')
    return version


def is_dirty() -> bool:
    """Whether the working tree has uncommitted changes.

    Checked separately rather than via `git describe --dirty`, because git
    refuses to combine `--dirty` with a commit-ish, and the describe calls
    below name one.
    """
    return bool(git("status", "--porcelain"))


def describe_gdh(rev: str = "HEAD") -> str | None:
    """The `git describe` output for the nearest gdh tag, or None if there is none.

    Fails cleanly before the first gdh tag exists: git exits 128 with
    "No names found, cannot describe anything."
    """
    out = git(
        "describe", "--tags", "--long", "--abbrev=" + ABBREV, "--match", "v*-gdh-*", rev,
        check=False,
    )
    return out or None


def distance_from_upstream_anchor(upstream: str, rev: str = "HEAD") -> tuple[int, str]:
    """Commits since the upstream release tag, for the pre-first-gdh-tag case.

    `git describe --match 'v*-gdh-*'` cannot supply a distance until a gdh tag
    exists, so count from the tag naming the upstream base instead. Counting
    from the repo root would be wrong -- that yields ~1800 here.
    """
    node = "g" + git("rev-parse", f"--short={ABBREV}", rev)
    anchor = f"v{upstream}"
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{anchor}", check=False):
        return int(git("rev-list", "--count", f"{anchor}..{rev}")), node
    return int(git("rev-list", "--count", rev)), node


def resolve(rev: str = "HEAD") -> dict:
    """Resolve the version at `rev`. See the module docstring for the scheme."""
    at_head = rev == "HEAD"
    upstream = upstream_version(None if at_head else rev)
    dirty = is_dirty() if at_head else False
    shallow = git("rev-parse", "--is-shallow-repository") == "true"
    described = describe_gdh(rev)

    source, tag, upstream_moved = "none", None, False
    major, minor = 0, 0
    if described:
        match = DESCRIBE_RE.match(described)
        if not match:
            raise VersionError(
                f"cannot parse gdh tag from `git describe` output {described!r}; "
                "expected v<upstream>-gdh-<major>.<minor>-<n>-g<sha>"
            )
        if match.group("upstream") == upstream:
            major, minor = int(match.group("major")), int(match.group("minor"))
            distance, node = int(match.group("distance")), "g" + match.group("node")
            source, tag = "tag", described.rsplit("-", 2)[0]
        else:
            # Upstream moved since the last gdh tag. The counters reset, so no
            # gdh release exists yet on this base unless the file below says so.
            upstream_moved = True
    if source == "none":
        distance, node = distance_from_upstream_anchor(upstream, rev)

    # The committed file, never the working tree: the anchor and the distance
    # describe commits, and an uncommitted edit is already reported as dirty.
    file_value = git("show", f"{rev}:{VERSION_FILE_NAME}", check=False) or None
    file_match = VERSION_RE.match(file_value or "")
    anchor = None
    if file_match and file_match.group("upstream") == upstream:
        # --first-parent: on main the PR's merge commit is what introduced the
        # file, and plain history simplification would name the commit inside
        # the PR instead.
        anchor = git(
            "log", "--first-parent", "-1", "--format=%H", rev, "--", VERSION_FILE_NAME,
            check=False,
        ) or None
        file_version = (int(file_match.group("major")), int(file_match.group("minor")))
        if anchor and file_version > (major, minor):
            major, minor = file_version
            # All ancestors, as `git describe` counts, so nothing changes when
            # the tag later lands on the anchor.
            distance = int(git("rev-list", "--count", f"{anchor}..{rev}"))
            node = "g" + git("rev-parse", f"--short={ABBREV}", rev)
            source = "file"

    # A shallow clone's grafted root looks like the commit that introduced the
    # file, so the file cannot make a shallow checkout exact.
    matched = source == "tag" or (source == "file" and not shallow)
    exact = matched and distance == 0 and not dirty

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
        "source": source,
        "tag": tag,
        "anchor": anchor,
        "shallow": shallow,
        "file": file_value,
    }


def scan_range(state: dict) -> str:
    """The `git log` range whose commits decide the next bump."""
    if state["source"] == "file":
        return f"{state['anchor']}..HEAD"
    if state["source"] == "tag":
        return f"{state['tag']}..HEAD"
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
    if state["source"] == "none":
        # No release on this upstream base yet: the GDH series (re)starts.
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
    if state["source"] == "none" and state["upstream_moved"]:
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
        raise VersionError("refusing to bump a dirty working tree; commit or clean it first")

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

    version = tag.lstrip("v")
    VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    git("add", "--", str(VERSION_FILE))
    git("commit", "-q", "-m", f"chore(version): {version}")
    print(f"committed {VERSION_FILE_NAME} = {version}")
    print(f"the PR's builds report {version}; after the merge, gdh-version-tag.yml "
          f"tags the merge commit {tag}")
    return 0


def push_tag(tag: str, anchor: str) -> None:
    """Push one tag, and only the tag. Another run winning the race is fine if
    it put the same tag on the same commit."""
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "push", "origin", f"refs/tags/{tag}"],
        capture_output=True, text=True,
    )
    if result.returncode == 0:
        print(f"pushed {tag} to origin")
        return
    git("fetch", "--quiet", "origin", f"+refs/tags/{tag}:refs/tags/{tag}", check=False)
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}", check=False) == anchor:
        print(f"{tag} is already on origin, on {anchor[:8]}")
        return
    raise VersionError(f"could not push {tag}: {result.stderr.strip()}")


def do_tag(args: argparse.Namespace) -> int:
    if git("rev-parse", "--is-shallow-repository") == "true":
        raise VersionError(
            "refusing to tag from a shallow clone: its grafted root can look like "
            "the commit that introduced .gdh-version"
        )
    state = resolve()
    anchor = state["anchor"]
    if not anchor:
        print(f"nothing to tag: no {VERSION_FILE_NAME} naming upstream {state['upstream']} at HEAD")
        return 0

    version = state["file"]
    tag = f"v{version}"
    existing = git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}", check=False)
    if existing == anchor:
        print(f"{tag} already names {anchor[:8]}, the commit that introduced {version}")
    elif existing:
        raise VersionError(
            f"{tag} already exists on {existing[:8]}, but {version} was introduced by "
            f"{anchor[:8]}; fix the tag by hand"
        )
    elif state["source"] != "file":
        raise VersionError(
            f"{VERSION_FILE_NAME} = {version} is not ahead of the nearest gdh tag "
            f"({state['tag']}), and {tag} does not exist; not tagging"
        )
    else:
        git("tag", "-a", tag, "-m", f"GDH release {version}", anchor)
        print(f"created {tag} on {anchor[:8]}")

    if args.push:
        push_tag(tag, anchor)
    return 0


def do_check_pr(args: argparse.Namespace) -> int:
    rev = args.rev
    base = git("merge-base", args.base, rev)
    before = git("show", f"{base}:{VERSION_FILE_NAME}", check=False, raw=True)
    after = git("show", f"{rev}:{VERSION_FILE_NAME}", check=False, raw=True)
    own = git(
        "rev-list", "--full-history", "--no-merges", f"{base}..{rev}", "--", VERSION_FILE_NAME,
    )

    if before == after:
        if own:
            # Two PRs bumped to the same number. Identical edits merge cleanly,
            # so the merge shows no change -- but this branch did set it.
            raise VersionError(
                f"this branch sets {VERSION_FILE_NAME} to {(after or '').strip()}, but "
                f"{args.base} already has that value: another PR took the number, or it "
                "merged earlier. Rebump: python3 scripts/gdh_version.py bump --level minor "
                "(or major)"
            )
        print(f"{VERSION_FILE_NAME} unchanged ({(after or 'absent').strip()}); nothing to check")
        return 0

    if after is None:
        raise VersionError(f"this branch deletes {VERSION_FILE_NAME}; releases need it")
    value = after.strip()
    match = VERSION_RE.match(value)
    if not match or after != value + "\n":
        raise VersionError(
            f"{VERSION_FILE_NAME} must be exactly one line, <upstream>-gdh-<major>.<minor>, "
            f"ending in LF; got {after!r}"
        )

    upstream = upstream_version(rev)
    if match.group("upstream") != upstream:
        raise VersionError(
            f"{VERSION_FILE_NAME} names upstream {match.group('upstream')}, "
            f"but vcpkg.json says {upstream}"
        )

    tag = f"v{value}"
    tagged = git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}", check=False)
    if tagged and not (is_ancestor(tagged, rev) and not is_ancestor(tagged, base)):
        raise VersionError(f"{tag} already exists (on {tagged[:8]}): {value} was already released")

    base_state = resolve(base)
    if base_state["upstream"] != upstream:
        allowed = {f"{upstream}-gdh-1.0": f"the first release on upstream {upstream}"}
    else:
        allowed = {
            next_tag(base_state, "major").lstrip("v"): "next major",
            next_tag(base_state, "minor").lstrip("v"): "next minor",
        }
    if value not in allowed:
        raise VersionError(
            f"{args.base} resolves {base_state['version']}; {VERSION_FILE_NAME} = {value} "
            f"is not a valid next step. expected one of: {', '.join(sorted(allowed))}"
        )
    print(f"{VERSION_FILE_NAME} {(before or 'absent').strip()} -> {value}: "
          f"{allowed[value]} of {base_state['version']}")
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

    bump = sub.add_parser("bump", help="commit the next version to .gdh-version on this branch")
    bump.add_argument("--level", choices=("auto", "major", "minor"), default="auto")
    bump.set_defaults(func=do_bump)

    tag = sub.add_parser("tag", help="tag the commit that introduced .gdh-version")
    tag.add_argument("--push", action="store_true", help="push the tag (only the tag) to origin")
    tag.set_defaults(func=do_tag)

    check_pr = sub.add_parser("check-pr", help="check a branch's .gdh-version change")
    check_pr.add_argument("--base", required=True, help="the branch the change merges into")
    check_pr.add_argument("--rev", default="HEAD", help="the revision to check (default HEAD)")
    check_pr.set_defaults(func=do_check_pr)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except VersionError as exc:
        print(f"gdh_version: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
