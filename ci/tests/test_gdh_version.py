#!/usr/bin/env python3
# test_gdh_version.py
#
# Tests for the GDH fork version scheme (scripts/gdh_version.py) and for the
# CMake block in CMakeLists.txt that has to agree with it.
#
# The version rule is expressed twice on purpose: CMake derives APP_VERSION at
# configure time without needing Python, and scripts/gdh_version.py drives the
# tooling. test_cmake_block_matches_script is what keeps the two from drifting.
#
# Run with:
#   python3 -m unittest -v ci.tests.test_gdh_version

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "gdh_version.py"
CMAKELISTS = REPO_ROOT / "CMakeLists.txt"

BEGIN_MARKER = "# --- gdh-version block: begin"
END_MARKER = "# --- gdh-version block: end ---"

VERSION_FILE_NAME = ".gdh-version"


def extract_cmake_block() -> str:
    """The gdh-version block from CMakeLists.txt, verbatim."""
    text = CMAKELISTS.read_text(encoding="utf-8")
    start = text.index(BEGIN_MARKER)
    end = text.index(END_MARKER) + len(END_MARKER)
    return text[start:end]


def resolve_with_cmake(source_dir: Path) -> str:
    """Run the extracted CMake block against `source_dir` and return APP_VERSION.

    This is how the no-git tiers get exercised: the block is the only thing a
    source tarball or a Nix build sandbox has to go on.
    """
    with tempfile.TemporaryDirectory(prefix="gdh-cmake-") as tmp:
        script = Path(tmp) / "resolve.cmake"
        script.write_text(
            f'set(CMAKE_SOURCE_DIR "{source_dir.as_posix()}")\n'
            f"{extract_cmake_block()}\n"
            'message(STATUS "APP_VERSION=${APP_VERSION}")\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            ["cmake", "-P", str(script)], capture_output=True, text=True
        )
    if result.returncode != 0:
        raise AssertionError(f"cmake -P failed: {result.stderr}")
    match = re.search(r"APP_VERSION=(\S+)", result.stderr + result.stdout)
    if match is None:
        raise AssertionError(f"no APP_VERSION in cmake output:\n{result.stderr}")
    return match.group(1)


def run_script(*args: str, script: Path | None = None) -> subprocess.CompletedProcess:
    """Invoke gdh_version.py.

    The script anchors on its own location, not on the working directory, so
    exercising a fixture repo means running the copy that lives inside it.
    """
    return subprocess.run(
        [sys.executable, str(script or SCRIPT), *args],
        capture_output=True,
        text=True,
    )


class FakeRepo:
    """A throwaway git repo with a vcpkg.json, for exercising the resolver.

    scripts/gdh_version.py anchors on its own location, so the fixture copies
    the script in rather than pointing it at a foreign tree.
    """

    def __init__(self, stack: unittest.TestCase, upstream: str = "3.2.8") -> None:
        self.root = Path(tempfile.mkdtemp(prefix="gdh-version-"))
        stack.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

        (self.root / "scripts").mkdir()
        self.script = self.root / "scripts" / "gdh_version.py"
        shutil.copy2(SCRIPT, self.script)
        self.set_upstream(upstream)

        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Test")
        self.commit("chore: initial")

    def git(self, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *args],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip()

    def set_upstream(self, version: str) -> None:
        (self.root / "vcpkg.json").write_text(
            json.dumps({"name": "tbc-tools", "version": version}, indent=2) + "\n",
            encoding="utf-8",
        )

    def commit(self, subject: str, body: str = "") -> str:
        # Touch a file so every commit is non-empty and ordering is stable.
        marker = self.root / "log.txt"
        marker.write_text(marker.read_text() + subject + "\n" if marker.exists() else subject + "\n")
        self.git("add", "-A")
        message = f"{subject}\n\n{body}" if body else subject
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def merge_commit(self, subject: str) -> None:
        """A merge commit, which the scan must ignore."""
        self.git("checkout", "-q", "-b", "side")
        self.commit("feat: work on the side branch")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", subject, "side")

    def tag(self, name: str) -> None:
        self.git("tag", "-a", name, "-m", name)

    def write_version_file(self, version: str) -> None:
        (self.root / VERSION_FILE_NAME).write_text(version + "\n", encoding="utf-8")

    def read_version_file(self) -> str:
        return (self.root / VERSION_FILE_NAME).read_text(encoding="utf-8").strip()

    def bump(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("bump", *args, script=self.script)

    def show(self, *args: str) -> str:
        result = run_script("show", *args, script=self.script)
        if result.returncode != 0:
            raise AssertionError(f"show failed: {result.stderr}")
        return result.stdout.strip()

    def resolution(self) -> dict:
        return json.loads(self.show("--json"))

    def propose(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("propose", *args, script=self.script)

    def tag_version(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("tag", *args, script=self.script)

    def check_pr(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("check-pr", *args, script=self.script)

    def commit_version(self, version: str, upstream: str | None = None) -> str:
        """Commit .gdh-version (and optionally a new upstream), as `bump` would."""
        if upstream is not None:
            self.set_upstream(upstream)
        self.write_version_file(version)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", f"chore(version): {version}")
        return self.git("rev-parse", "HEAD")

    def pr_merge(self, work, subject: str = "Merge pull request #1 from GDH-Technologies/side") -> tuple[str, str]:
        """Run `work(repo)` on a side branch and merge it --no-ff, as the merge button does.

        Returns (merge commit, side tip).
        """
        self.git("checkout", "-q", "-b", "side")
        work(self)
        side = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", subject, "side")
        self.git("branch", "-q", "-D", "side")
        return self.git("rev-parse", "HEAD"), side

    def add_bare_remote(self, stack: unittest.TestCase) -> Path:
        remote = Path(tempfile.mkdtemp(prefix="gdh-origin-"))
        stack.addCleanup(shutil.rmtree, remote, ignore_errors=True)
        subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True)
        self.git("remote", "add", "origin", str(remote))
        return remote

    def shallow_clone(self, stack: unittest.TestCase) -> "FakeRepo":
        """A depth-1 clone of this repo, with the script copied in like the original."""
        clone = object.__new__(FakeRepo)
        clone.root = Path(tempfile.mkdtemp(prefix="gdh-shallow-"))
        stack.addCleanup(shutil.rmtree, clone.root, ignore_errors=True)
        subprocess.run(
            ["git", "clone", "-q", "--depth", "1", f"file://{self.root}", str(clone.root)],
            check=True, capture_output=True,
        )
        clone.script = clone.root / "scripts" / "gdh_version.py"
        return clone


class TestVersionResolution(unittest.TestCase):
    def test_no_gdh_tag_reports_the_zero_sentinel(self):
        repo = FakeRepo(self)
        state = repo.resolution()
        self.assertEqual((state["major"], state["minor"]), (0, 0))
        self.assertFalse(state["exact"])
        self.assertRegex(state["version"], r"^3\.2\.8-gdh-0\.0\+\d+\.g[0-9a-f]{8}$")

    def test_exactly_on_a_tag_is_bare(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        self.assertEqual(repo.show(), "3.2.8-gdh-1.0")

    def test_ahead_of_a_tag_carries_distance_and_node(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        repo.commit("fix: something")
        repo.commit("fix: something else")
        self.assertRegex(repo.show(), r"^3\.2\.8-gdh-1\.0\+2\.g[0-9a-f]{8}$")

    def test_dirty_tree_is_marked(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        (repo.root / "untracked.txt").write_text("scratch\n")
        self.assertTrue(repo.show().endswith(".dirty"))

    def test_upstream_move_resets_to_the_zero_sentinel(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        repo.commit("fix: after the release")
        repo.set_upstream("3.2.9")
        repo.commit("chore: merge upstream 3.2.9")

        state = repo.resolution()
        self.assertTrue(state["upstream_moved"])
        self.assertEqual(state["upstream"], "3.2.9")
        self.assertEqual((state["major"], state["minor"]), (0, 0))
        self.assertTrue(state["version"].startswith("3.2.9-gdh-0.0+"))

    def test_distance_anchors_on_the_upstream_tag_not_the_repo_root(self):
        """The pre-first-gdh-tag distance must not count the whole history.

        In the real repo that difference is 40 vs ~1800.
        """
        repo = FakeRepo(self)
        repo.commit("chore: one")
        repo.commit("chore: two")
        repo.tag("v3.2.8")
        repo.commit("feat: after upstream")
        self.assertEqual(repo.resolution()["distance"], 1)


class TestVersionFileAheadOfTheTags(unittest.TestCase):
    """A committed .gdh-version newer than every gdh tag is the version.

    A bump rides inside a PR, and the merge button creates the commit that ends
    up on main, so no tag can exist when the PR's builds (or the merge's) run.
    The file is exact on the first-parent commit that introduced it; the tag
    that gdh-version-tag.yml adds afterwards is a record, not an input.
    """

    def test_a_newer_version_file_is_exact_on_the_commit_that_introduced_it(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        anchor = repo.commit_version("3.2.8-gdh-2.1")
        state = repo.resolution()
        self.assertEqual(state["version"], "3.2.8-gdh-2.1")
        self.assertTrue(state["exact"])
        self.assertEqual(state["source"], "file")
        self.assertEqual(state["anchor"], anchor)

    def test_commits_after_the_file_carry_distance_from_it(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")
        repo.commit("fix: one")
        repo.commit("fix: two")
        self.assertRegex(repo.show(), r"^3\.2\.8-gdh-2\.1\+2\.g[0-9a-f]{8}$")

    def test_the_first_bump_needs_no_earlier_tag(self):
        repo = FakeRepo(self)
        repo.commit_version("3.2.8-gdh-1.0")
        self.assertEqual(repo.show(), "3.2.8-gdh-1.0")

    def test_distance_is_continuous_when_the_tag_appears(self):
        """Tagging the anchor later must not change any commit's version."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        anchor = repo.commit_version("3.2.8-gdh-2.1")
        repo.commit("fix: after the bump")
        before = repo.show()
        repo.git("tag", "-a", "v3.2.8-gdh-2.1", "-m", "GDH release 3.2.8-gdh-2.1", anchor)
        self.assertEqual(repo.show(), before)
        self.assertEqual(repo.resolution()["source"], "tag")

    def test_the_pr_merge_commit_is_the_anchor(self):
        """On main the merge commit introduces the file on the first-parent
        line, so it is the exact commit -- not the version commit inside the PR."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        merge, side = repo.pr_merge(lambda r: (r.commit("feat: a thing"), r.commit_version("3.2.8-gdh-2.1")))
        state = repo.resolution()
        self.assertEqual(state["version"], "3.2.8-gdh-2.1")
        self.assertEqual(state["anchor"], merge)
        self.assertNotEqual(state["anchor"], side)

    def test_the_branch_itself_is_exact_before_the_merge(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.git("checkout", "-q", "-b", "side")
        repo.commit_version("3.2.8-gdh-2.1")
        self.assertEqual(repo.show(), "3.2.8-gdh-2.1")

    def test_major_and_minor_compare_numerically(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.9")
        repo.commit_version("3.2.8-gdh-2.10")
        self.assertEqual(repo.show(), "3.2.8-gdh-2.10")

        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-9.0")
        repo.commit_version("3.2.8-gdh-10.0")
        self.assertEqual(repo.show(), "3.2.8-gdh-10.0")

    def test_an_older_file_still_loses_to_the_tag(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-1.0")
        self.assertRegex(repo.show(), r"^3\.2\.8-gdh-2\.0\+1\.g[0-9a-f]{8}$")

    def test_a_file_naming_another_upstream_is_ignored(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.9-gdh-5.0")
        self.assertRegex(repo.show(), r"^3\.2\.8-gdh-2\.0\+1\.g[0-9a-f]{8}$")

    def test_a_sync_can_carry_the_new_series(self):
        """An upstream sync resets the counters; carrying <new>-gdh-1.0 in the
        same PR makes it exact at once instead of a gdh-0.0 sentinel."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        repo.commit_version("3.2.9-gdh-1.0", upstream="3.2.9")
        state = repo.resolution()
        self.assertEqual(state["version"], "3.2.9-gdh-1.0")
        self.assertTrue(state["exact"])

    def test_the_committed_value_is_read_not_the_working_tree(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")
        repo.write_version_file("3.2.8-gdh-7.0")
        self.assertRegex(repo.show(), r"^3\.2\.8-gdh-2\.1\+0\.g[0-9a-f]{8}\.dirty$")

    def test_a_shallow_clone_is_never_exact_from_the_file(self):
        """A shallow clone's grafted root looks like the commit that added the
        file; that must not pass for the real anchor."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")
        repo.commit("fix: after the bump")
        clone = repo.shallow_clone(self)
        state = clone.resolution()
        self.assertTrue(state["shallow"])
        self.assertFalse(state["exact"])
        self.assertTrue(state["version"].startswith("3.2.8-gdh-2.1+"))


class TestBumpClassification(unittest.TestCase):
    def test_fix_only_proposes_no_bump(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        repo.commit("fix: a bug")
        repo.commit("docs: a note")
        result = repo.propose()
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("no bump", result.stdout)

    def test_feat_proposes_minor(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        repo.commit("feat: a new thing")
        result = repo.propose()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("proposed level   minor", result.stdout)
        self.assertIn("v3.2.8-gdh-1.1", result.stdout)

    def test_bang_proposes_major(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.3")
        repo.commit("feat!: rename a flag")
        result = repo.propose()
        self.assertIn("proposed level   major", result.stdout)
        self.assertIn("v3.2.8-gdh-2.0", result.stdout)

    def test_breaking_change_trailer_proposes_major(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        repo.commit("refactor: rework the export path", body="BREAKING CHANGE: --out is gone")
        result = repo.propose()
        self.assertIn("proposed level   major", result.stdout)

    def test_merge_commits_are_ignored(self):
        """Merge subjects carry no type; the commits they bring in do."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        repo.merge_commit("Merge pull request #1 from GDH-Technologies/side")
        result = repo.propose()
        # The side branch's own `feat:` commit still counts...
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("proposed level   minor", result.stdout)
        # ...but the merge subject itself is not among the reasons.
        self.assertNotIn("Merge pull request", result.stdout)

    def test_upstream_move_forces_reset_to_one_zero(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        repo.set_upstream("3.2.9")
        repo.commit("fix: only a fix")
        # Even a fix-only range resets, because the base changed.
        result = repo.propose("--level", "minor")
        self.assertIn("v3.2.9-gdh-1.0", result.stdout)

    def test_forced_level_overrides_an_empty_scan(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        repo.commit("fix: a bug")
        result = repo.propose("--level", "major")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("(forced)", result.stdout)
        self.assertIn("v3.2.8-gdh-2.0", result.stdout)

    def test_propose_starts_from_a_file_version_ahead_of_the_tags(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        anchor = repo.commit_version("3.2.8-gdh-2.1")
        repo.commit("feat: after the bump")
        result = repo.propose()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"scanned range    {anchor}..HEAD", result.stdout)
        self.assertIn("v3.2.8-gdh-2.2", result.stdout)

    def test_propose_after_a_merged_bump_proposes_nothing(self):
        """Without the file tier this re-proposed the version just merged."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.pr_merge(lambda r: (r.commit("feat: a thing"), r.commit_version("3.2.8-gdh-2.1")))
        result = repo.propose()
        self.assertEqual(result.returncode, 2, result.stdout)

    def test_propose_after_a_sync_that_carried_one_zero_proposes_one_one(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        repo.commit_version("3.2.9-gdh-1.0", upstream="3.2.9")
        repo.commit("feat: new work")
        result = repo.propose()
        self.assertIn("v3.2.9-gdh-1.1", result.stdout)


class TestCMakeParity(unittest.TestCase):
    """The CMake block and the script must resolve the same version."""

    def test_markers_are_present_and_ordered(self):
        text = CMAKELISTS.read_text(encoding="utf-8")
        self.assertEqual(text.count(BEGIN_MARKER), 1)
        self.assertEqual(text.count(END_MARKER), 1)
        self.assertLess(text.index(BEGIN_MARKER), text.index(END_MARKER))

    @unittest.skipIf(shutil.which("cmake") is None, "cmake is not installed")
    def test_cmake_block_matches_script(self):
        expected = run_script("show").stdout.strip()
        self.assertEqual(resolve_with_cmake(REPO_ROOT), expected)


@unittest.skipIf(shutil.which("cmake") is None, "cmake is not installed")
class TestVersionFileTier(unittest.TestCase):
    """`.gdh-version` is the tier for consumers that cannot see git tags.

    Nix copies only tracked files into the build sandbox and flakes never
    expose tags, so without this tier a Nix build silently reports upstream's
    version instead of the fork's.
    """

    def make_tarball(self, upstream: str = "3.2.8", version: str | None = None) -> Path:
        """A source tree with no .git, as a tarball or a Nix sandbox sees it."""
        root = Path(tempfile.mkdtemp(prefix="gdh-tarball-"))
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        (root / "vcpkg.json").write_text(
            json.dumps({"name": "tbc-tools", "version": upstream}, indent=2) + "\n",
            encoding="utf-8",
        )
        if version is not None:
            (root / VERSION_FILE_NAME).write_text(version + "\n", encoding="utf-8")
        return root

    def test_version_file_is_used_when_there_is_no_git(self):
        root = self.make_tarball(version="3.2.8-gdh-1.0")
        self.assertEqual(resolve_with_cmake(root), "3.2.8-gdh-1.0")

    def test_falls_back_to_vcpkg_when_there_is_no_version_file(self):
        root = self.make_tarball()
        self.assertEqual(resolve_with_cmake(root), "3.2.8")

    def test_a_stale_version_file_is_ignored(self):
        """A file naming a different upstream base is stale, exactly as a
        stale tag is: upstream moved and the GDH counters reset."""
        root = self.make_tarball(upstream="3.2.9", version="3.2.8-gdh-1.0")
        self.assertEqual(resolve_with_cmake(root), "3.2.9")

    def test_a_malformed_version_file_is_ignored(self):
        root = self.make_tarball(version="not a version")
        self.assertEqual(resolve_with_cmake(root), "3.2.8")

    def test_an_empty_version_file_is_ignored(self):
        root = self.make_tarball(version="")
        self.assertEqual(resolve_with_cmake(root), "3.2.8")

    def test_git_wins_over_the_version_file(self):
        """In a git checkout the tag is authoritative; the file is a cache of
        it and must never override the live describe (which carries distance
        and dirty state the file cannot)."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.write_version_file("3.2.8-gdh-1.0")
        repo.git("add", "-A")
        repo.git("commit", "-q", "-m", "chore: stale version file")
        self.assertRegex(
            resolve_with_cmake(repo.root), r"^3\.2\.8-gdh-2\.0\+1\.g[0-9a-f]{8}$"
        )


class TestBumpWritesTheVersionFile(unittest.TestCase):
    def test_bump_commits_the_file_and_does_not_tag(self):
        """The bump rides in a PR; gdh-version-tag.yml tags the merge on main."""
        repo = FakeRepo(self)
        repo.commit("feat: something worth releasing")
        result = repo.bump()
        self.assertEqual(result.returncode, 0, result.stderr)

        self.assertEqual(repo.read_version_file(), "3.2.8-gdh-1.0")
        self.assertEqual(repo.show(), "3.2.8-gdh-1.0")
        self.assertEqual(repo.git("tag", "-l"), "")
        self.assertEqual(repo.git("log", "-1", "--format=%s"), "chore(version): 3.2.8-gdh-1.0")
        self.assertEqual(repo.git("status", "--porcelain"), "")

    def test_bump_has_no_push_option(self):
        repo = FakeRepo(self)
        repo.commit("feat: something")
        result = repo.bump("--push")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((repo.root / VERSION_FILE_NAME).exists())

    def test_bump_refuses_a_dirty_tree_before_writing_anything(self):
        repo = FakeRepo(self)
        repo.commit("feat: something")
        (repo.root / "untracked.txt").write_text("scratch\n")
        result = repo.bump()
        self.assertEqual(result.returncode, 1)
        self.assertFalse((repo.root / VERSION_FILE_NAME).exists())

    def test_a_second_bump_updates_the_file(self):
        repo = FakeRepo(self)
        repo.commit("feat: first")
        self.assertEqual(repo.bump().returncode, 0)
        repo.commit("feat: second")
        self.assertEqual(repo.bump().returncode, 0)
        self.assertEqual(repo.read_version_file(), "3.2.8-gdh-1.1")
        self.assertEqual(repo.show(), "3.2.8-gdh-1.1")


class TestTag(unittest.TestCase):
    """`tag` records a version: it tags the anchor, never HEAD, and only once."""

    def tag_commit(self, repo: FakeRepo, name: str) -> str:
        return repo.git("rev-parse", f"refs/tags/{name}^{{commit}}")

    def test_tag_lands_on_the_anchor_not_head(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        anchor = repo.commit_version("3.2.8-gdh-2.1")
        repo.commit("fix: after the bump")
        result = repo.tag_version()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.tag_commit(repo, "v3.2.8-gdh-2.1"), anchor)
        self.assertEqual(repo.git("cat-file", "-t", "refs/tags/v3.2.8-gdh-2.1"), "tag")
        self.assertEqual(
            repo.git("tag", "-l", "--format=%(contents:subject)", "v3.2.8-gdh-2.1"),
            "GDH release 3.2.8-gdh-2.1",
        )

    def test_tag_is_idempotent(self):
        repo = FakeRepo(self)
        repo.commit_version("3.2.8-gdh-1.0")
        self.assertEqual(repo.tag_version().returncode, 0)
        again = repo.tag_version()
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn("already", again.stdout)

    def test_tag_refuses_a_tag_that_exists_elsewhere(self):
        repo = FakeRepo(self)
        repo.commit_version("3.2.8-gdh-1.0")
        repo.commit("fix: later")
        repo.git("tag", "v3.2.8-gdh-1.0")  # lightweight, on HEAD, not the anchor
        result = repo.tag_version()
        self.assertEqual(result.returncode, 1)
        self.assertIn("exists", result.stderr)

    def test_tag_anchors_on_the_merge_commit(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        merge, _ = repo.pr_merge(lambda r: r.commit_version("3.2.8-gdh-2.1"))
        self.assertEqual(repo.tag_version().returncode, 0)
        self.assertEqual(self.tag_commit(repo, "v3.2.8-gdh-2.1"), merge)

    def test_tag_push_sends_only_the_tag(self):
        repo = FakeRepo(self)
        remote = repo.add_bare_remote(self)
        repo.git("push", "-q", "origin", "main")
        remote_main = repo.git("rev-parse", "HEAD")
        anchor = repo.commit_version("3.2.8-gdh-1.0")  # local main is now ahead
        result = repo.tag_version("--push")
        self.assertEqual(result.returncode, 0, result.stderr)

        def at_remote(ref: str) -> str:
            return subprocess.run(
                ["git", "-C", str(remote), "rev-parse", ref],
                capture_output=True, text=True, check=True,
            ).stdout.strip()

        self.assertEqual(at_remote("refs/tags/v3.2.8-gdh-1.0^{commit}"), anchor)
        self.assertEqual(at_remote("refs/heads/main"), remote_main)

    def test_tag_is_a_no_op_without_a_usable_file(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-1.0")
        result = repo.tag_version()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nothing to tag", result.stdout)
        self.assertEqual(repo.git("tag", "-l"), "v3.2.8-gdh-1.0")

    def test_tag_refuses_a_shallow_clone(self):
        repo = FakeRepo(self)
        repo.commit_version("3.2.8-gdh-1.0")
        repo.commit("fix: later")
        result = repo.shallow_clone(self).tag_version()
        self.assertEqual(result.returncode, 1)
        self.assertIn("shallow", result.stderr)


class TestCheckPr(unittest.TestCase):
    """The guardrail on a PR's .gdh-version: a valid next step, or unchanged."""

    def branch(self, repo: FakeRepo) -> None:
        repo.git("checkout", "-q", "-b", "side")

    def test_an_unchanged_file_passes(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        self.branch(repo)
        repo.commit("fix: nothing to do with versions")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("unchanged", result.stdout)

    def test_the_next_minor_passes(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        self.branch(repo)
        repo.commit_version("3.2.8-gdh-2.1")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("next minor", result.stdout)

    def test_the_next_major_passes(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        self.branch(repo)
        repo.commit_version("3.2.8-gdh-3.0")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("next major", result.stdout)

    def test_a_skipped_number_fails(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        self.branch(repo)
        repo.commit_version("3.2.8-gdh-2.2")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected one of", result.stderr)

    def test_a_malformed_or_non_canonical_file_fails(self):
        for content in (b"3.2.8-gdh-2.1\r\n", b"3.2.8-gdh-2.1 \n", b"3.2.8-gdh-2.1", b"2.1\n"):
            with self.subTest(content=content):
                repo = FakeRepo(self)
                repo.tag("v3.2.8-gdh-2.0")
                self.branch(repo)
                (repo.root / VERSION_FILE_NAME).write_bytes(content)
                repo.git("add", "-A")
                repo.git("commit", "-q", "-m", "chore(version): odd")
                result = repo.check_pr("--base", "main")
                self.assertEqual(result.returncode, 1, result.stdout)

    def test_the_wrong_upstream_fails(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        self.branch(repo)
        repo.commit_version("3.2.9-gdh-2.1")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 1)
        self.assertIn("upstream", result.stderr)

    def test_an_upstream_move_requires_one_zero(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        self.branch(repo)
        repo.commit_version("3.2.9-gdh-1.0", upstream="3.2.9")
        self.assertEqual(repo.check_pr("--base", "main").returncode, 0)

        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        self.branch(repo)
        repo.commit_version("3.2.9-gdh-4.3", upstream="3.2.9")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 1)
        self.assertIn("3.2.9-gdh-1.0", result.stderr)

    def test_a_racing_pr_must_rebump(self):
        """Two PRs bump to the same number; identical edits merge cleanly, so
        the second PR's merge ref shows no change at all. Its own commits do."""
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.git("checkout", "-q", "-b", "pr-b")
        # Its own file, so the only overlap with A is the identical version edit.
        (repo.root / "b.txt").write_text("work in B\n")
        repo.git("add", "-A")
        repo.git("commit", "-q", "-m", "feat: work in B")
        repo.commit_version("3.2.8-gdh-2.1")
        repo.git("checkout", "-q", "main")
        repo.pr_merge(lambda r: (r.commit("feat: work in A"), r.commit_version("3.2.8-gdh-2.1")))
        # refs/pull/N/merge for B: main as it stands now, merged with B.
        repo.git("checkout", "-q", "--detach", "main")
        repo.git("merge", "-q", "--no-ff", "-m", "Merge pr-b into main", "pr-b")
        result = repo.check_pr("--base", "HEAD^1")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("rebump", result.stderr.lower())

    def test_a_number_already_tagged_elsewhere_fails(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.git("checkout", "-q", "-b", "abandoned")
        repo.commit("feat: never merged")
        repo.tag("v3.2.8-gdh-2.1")
        repo.git("checkout", "-q", "main")
        self.branch(repo)
        repo.commit_version("3.2.8-gdh-2.1")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 1)
        self.assertIn("already", result.stderr)

    def test_a_base_resolved_from_an_untagged_file(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")  # merged, not yet tagged
        self.branch(repo)
        repo.commit_version("3.2.8-gdh-2.2")
        self.assertEqual(repo.check_pr("--base", "main").returncode, 0)

    def test_deleting_the_file_fails(self):
        repo = FakeRepo(self)
        repo.commit_version("3.2.8-gdh-1.0")
        self.branch(repo)
        repo.git("rm", "-q", VERSION_FILE_NAME)
        repo.git("commit", "-q", "-m", "chore: drop the version file")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 1)
        self.assertIn("delet", result.stderr)

    def test_a_branch_behind_main_is_not_flagged(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        self.branch(repo)
        repo.commit_version("3.2.8-gdh-2.1")
        repo.git("checkout", "-q", "main")
        repo.commit("fix: main moved on")
        repo.git("checkout", "-q", "side")
        self.assertEqual(repo.check_pr("--base", "main").returncode, 0)


@unittest.skipIf(shutil.which("cmake") is None, "cmake is not installed")
class TestCMakeParityInFixtures(unittest.TestCase):
    """The CMake block and the script agree in every file-tier scenario."""

    def assert_parity(self, repo: FakeRepo) -> None:
        self.assertEqual(resolve_with_cmake(repo.root), repo.show())

    def test_file_ahead_of_the_tag(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")
        self.assert_parity(repo)
        repo.commit("fix: later")
        self.assert_parity(repo)

    def test_pr_merge(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.pr_merge(lambda r: (r.commit("feat: a"), r.commit_version("3.2.8-gdh-2.1")))
        self.assert_parity(repo)

    def test_numeric_compare(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.9")
        repo.commit_version("3.2.8-gdh-2.10")
        self.assert_parity(repo)

    def test_older_file(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-1.0")
        self.assert_parity(repo)

    def test_sync_carrying_one_zero(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-4.2")
        repo.commit_version("3.2.9-gdh-1.0", upstream="3.2.9")
        self.assert_parity(repo)

    def test_dirty(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")
        repo.write_version_file("3.2.8-gdh-7.0")
        self.assert_parity(repo)

    def test_shallow(self):
        repo = FakeRepo(self)
        repo.tag("v3.2.8-gdh-2.0")
        repo.commit_version("3.2.8-gdh-2.1")
        repo.commit("fix: later")
        self.assert_parity(repo.shallow_clone(self))


STUB_PROJECT = """cmake_minimum_required(VERSION 3.20)
project(gdh_version_stub NONE)
set(APP_BRANCH "nix" CACHE STRING "Build source identifier")
set(APP_COMMIT "0.0.0" CACHE STRING "Build version identifier")
{block}
message(STATUS "APP_VERSION=${{APP_VERSION}}")
message(STATUS "APP_BRANCH=${{APP_BRANCH}}")
message(STATUS "APP_COMMIT=${{APP_COMMIT}}")
"""


@unittest.skipIf(shutil.which("cmake") is None, "cmake is not installed")
class TestConfigureDoesNotFreeze(unittest.TestCase):
    """A real configure of a project carrying the block.

    APP_VERSION used to be written back to the cache, and the block only ran
    when it was unset -- so a reused build directory (win0's) kept the version
    of its first configure forever.
    """

    def project(self) -> tuple[FakeRepo, Path]:
        repo = FakeRepo(self)
        (repo.root / "CMakeLists.txt").write_text(
            STUB_PROJECT.format(block=extract_cmake_block()), encoding="utf-8"
        )
        repo.git("add", "-A")
        repo.git("commit", "-q", "-m", "chore: stub project")
        build = Path(tempfile.mkdtemp(prefix="gdh-build-"))
        self.addCleanup(shutil.rmtree, build, ignore_errors=True)
        return repo, build

    def configure(self, repo: FakeRepo, build: Path, *args: str) -> dict:
        result = subprocess.run(
            ["cmake", "-S", str(repo.root), "-B", str(build), *args],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return dict(re.findall(r"-- (APP_\w+)=(\S*)", result.stdout))

    def test_a_reconfigure_picks_up_a_new_version(self):
        repo, build = self.project()
        repo.tag("v3.2.8-gdh-1.0")
        self.assertEqual(self.configure(repo, build)["APP_VERSION"], "3.2.8-gdh-1.0")
        repo.commit_version("3.2.8-gdh-1.1")
        self.assertEqual(self.configure(repo, build)["APP_VERSION"], "3.2.8-gdh-1.1")
        self.assertNotIn("APP_VERSION:", (build / "CMakeCache.txt").read_text())

    def test_the_legacy_cached_entry_is_discarded(self):
        repo, build = self.project()
        repo.tag("v3.2.8-gdh-1.0")
        init = build / "legacy-cache.cmake"
        init.write_text('set(APP_VERSION "stale" CACHE STRING "Release version identifier")\n')
        values = self.configure(repo, build, "-C", str(init))
        self.assertEqual(values["APP_VERSION"], "3.2.8-gdh-1.0")
        self.assertNotIn("APP_VERSION:", (build / "CMakeCache.txt").read_text())

    def test_an_explicit_version_wins(self):
        repo, build = self.project()
        repo.tag("v3.2.8-gdh-1.0")
        self.assertEqual(self.configure(repo, build, "-DAPP_VERSION=9.9.9")["APP_VERSION"], "9.9.9")

    def test_build_identity_comes_from_git(self):
        repo, build = self.project()
        values = self.configure(repo, build)
        tree = repo.git("rev-parse", "HEAD^{tree}")[:12]
        self.assertEqual(values["APP_BRANCH"], "git")
        self.assertEqual(values["APP_COMMIT"], f"tree-{tree}")

    def test_an_explicit_build_identity_wins(self):
        repo, build = self.project()
        values = self.configure(repo, build, "-DAPP_COMMIT=src-abc")
        self.assertEqual(values["APP_COMMIT"], "src-abc")
        self.assertEqual(values["APP_BRANCH"], "nix")


if __name__ == "__main__":
    unittest.main()
