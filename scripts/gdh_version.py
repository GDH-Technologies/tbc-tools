#!/usr/bin/env python3
# gdh_version.py
#
# Resolve the GDH fork version of tbc-tools, and cut releases on main.
#
# This fork carries GDH-only work on top of harrypm/tbc-tools, and needs to say
# so without colonising upstream's version namespace. The scheme is:
#
#     version   <UPSTREAM>-gdh-<MAJOR>.<MINOR>           exactly on the release commit, clean tree
#               <UPSTREAM>-gdh-<MAJOR>.<MINOR>+<N>.g<sha>[.dirty]   N commits after it
#     tag       v<UPSTREAM>-gdh-<MAJOR>.<MINOR>          e.g. v3.2.9-gdh-2.1
#
# <UPSTREAM> is upstream's OWN declared version -- the "version" field of
# vcpkg.json, which upstream syncs keep current. The GDH path never writes it.
#
# Releases happen on main, never in a PR (.github/workflows/gdh-release.yml has
# the whole process). After a merge that changed shipped code (SHIPPED_PATHS),
# `release` writes .gdh-version, commits `chore(release): <version>`, and tags
# that commit v<version>. The committed file is what a Nix build reads (it sees
# no .git); CMake and `show` read the tag, and agree with it.
#
# Usage:
#   scripts/gdh_version.py show [--json]           # the resolved version
#   scripts/gdh_version.py next [--json]           # what the next release would be
#   scripts/gdh_version.py release [--push]        # cut it (gdh-release.yml, on main)
#   scripts/gdh_version.py check-pr --base B       # PRs must not touch .gdh-version
#
# `next` and `release` take --level auto|minor|major. auto releases only when
# shipped code changed since the last release, as minor, or major when a commit
# is marked breaking (`type!:` or a `BREAKING CHANGE:` trailer). When upstream's
# version moved, the series restarts at <upstream>-gdh-1.0. minor/major force a
# release even with nothing shipped (a rebuild).
#
# Exit codes: 0 on success; 2 when auto finds nothing to release; 1 on an error
# or a failed check.
#
# NOTE for CMake consumers: CMakeLists.txt derives APP_VERSION with the same
# rule at configure time, and ci/gdh/test_gdh_version.py holds the two in
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

# What ships: a change under any of these is what makes a release. Kept equal
# to the `paths` of gdh-release.yml and the platform workflows by
# ci/gdh/check_ci.py. .gdh-version itself is not here: a release commit changes
# only that, and must not need another release.
SHIPPED_PATHS = (
    "src/",
    "scripts/",
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "flake.nix",
    "flake.lock",
)

# Conventional Commits: a `!` before the colon, or a BREAKING CHANGE trailer,
# makes the release a major one.
BREAKING_SUBJECT_RE = re.compile(r"^[a-zA-Z]+(?:\([^)]*\))?!:")
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


def last_release(rev: str = "HEAD") -> str | None:
    """The nearest v*-gdh-* tag reachable from `rev`, or None before the first."""
    return git("describe", "--tags", "--abbrev=0", "--match", "v*-gdh-*", rev, check=False) or None


def shipped_changes(since: str | None, rev: str = "HEAD") -> list[dict]:
    """The non-merge commits after `since` that changed shipped code, oldest first.

    Merge commits are skipped: a PR's merge commit adds nothing its own commits
    did not, and its subject carries no Conventional Commit type.
    """
    commit_range = f"{since}..{rev}" if since else rev
    raw = git(
        "log", "--no-merges", "--reverse", "--format=%H%x00%s%x00%b%x1e", commit_range,
        "--", *SHIPPED_PATHS,
    )
    commits = []
    for record in raw.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        sha, subject, body = (record.split("\x00", 2) + ["", ""])[:3]
        breaking = bool(BREAKING_SUBJECT_RE.match(subject) or BREAKING_BODY_RE.search(body))
        commits.append({"sha": sha, "subject": subject, "breaking": breaking})
    return commits


def plan_release(level: str = "auto", rev: str = "HEAD") -> dict:
    """What a release at `level` would be: the new version and the commits it ships.

    auto: no shipped change since the last release means no release; any
    breaking one means major; otherwise minor.
    """
    state = resolve(rev)
    since = last_release(rev)
    commits = shipped_changes(since, rev)
    if level == "auto":
        if not commits:
            level = "none"
        elif any(commit["breaking"] for commit in commits):
            level = "major"
        else:
            level = "minor"
    version = next_tag(state, level).lstrip("v") if level != "none" else None
    return {
        "current": state["version"],
        "since": since,
        "level": level,
        "version": version,
        "upstream_moved": state["upstream_moved"],
        "commits": commits,
    }


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


def do_next(args: argparse.Namespace) -> int:
    plan = plan_release(args.level)
    if args.json:
        print(json.dumps(plan, indent=2))
    else:
        print(f"current version  {plan['current']}")
        print(f"last release     {plan['since'] or '(none)'}")
        if plan["upstream_moved"]:
            print("note             upstream moved since the last release; the series restarts at 1.0")
        if plan["commits"]:
            print("shipped changes:")
            for commit in plan["commits"]:
                mark = "  BREAKING" if commit["breaking"] else ""
                print(f"  {commit['sha'][:8]}  {commit['subject']}{mark}")
        else:
            print("shipped changes  none")
        if plan["version"]:
            print(f"next release     {plan['version']}  ({plan['level']})")
        else:
            print("next release     none -- nothing shipped changed since the last release")
    return 2 if plan["level"] == "none" else 0


def do_release(args: argparse.Namespace) -> int:
    if is_dirty():
        raise VersionError("refusing to release from a dirty working tree")
    plan = plan_release(args.level)
    if plan["level"] == "none":
        print("nothing to release: no shipped change since the last release")
        return 2

    version = plan["version"]
    tag = f"v{version}"
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}", check=False):
        raise VersionError(f"tag {tag} already exists")

    VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    git("add", "--", VERSION_FILE_NAME)
    body = "\n".join(f"- {c['sha'][:8]} {c['subject']}" for c in plan["commits"])
    message = f"chore(release): {version}"
    if body:
        message += f"\n\nShips:\n{body}"
    git("commit", "-q", "-m", message)
    git("tag", "-a", tag, "-m", f"GDH release {version}")
    print(f"released {version} ({plan['level']}): commit {git('rev-parse', '--short=8', 'HEAD')}, tag {tag}")

    if args.push:
        # Both or neither: a release commit without its tag, or a tag without
        # its commit on main, is the one state this must never leave behind.
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "push", "--atomic", "origin",
             f"HEAD:refs/heads/{args.branch}", f"refs/tags/{tag}"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise VersionError(f"could not push {tag} to {args.branch}: {result.stderr.strip()}")
        print(f"pushed the release commit to {args.branch} and {tag} to origin")
    return 0


def do_check_pr(args: argparse.Namespace) -> int:
    base = git("merge-base", args.base, args.rev)
    before = git("show", f"{base}:{VERSION_FILE_NAME}", check=False, raw=True)
    after = git("show", f"{args.rev}:{VERSION_FILE_NAME}", check=False, raw=True)
    if before != after:
        raise VersionError(
            f"this PR changes {VERSION_FILE_NAME}; only the release workflow writes it "
            "(gdh-release.yml cuts a release after the merge). Drop the change."
        )
    print(f"{VERSION_FILE_NAME} untouched; the release happens on main after the merge")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    show = sub.add_parser("show", help="print the resolved version")
    show.add_argument("--json", action="store_true", help="print the full resolution")
    show.set_defaults(func=do_show)

    nxt = sub.add_parser("next", help="print what the next release would be")
    nxt.add_argument("--level", choices=("auto", "major", "minor"), default="auto")
    nxt.add_argument("--json", action="store_true", help="print the plan as JSON")
    nxt.set_defaults(func=do_next)

    release = sub.add_parser("release", help="commit .gdh-version and tag the release")
    release.add_argument("--level", choices=("auto", "major", "minor"), default="auto")
    release.add_argument("--push", action="store_true",
                         help="push the commit and the tag to origin, atomically")
    release.add_argument("--branch", default="main", help="the branch to push to (default main)")
    release.set_defaults(func=do_release)

    check_pr = sub.add_parser("check-pr", help="fail if a PR changes .gdh-version")
    check_pr.add_argument("--base", required=True, help="the branch the PR merges into")
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
