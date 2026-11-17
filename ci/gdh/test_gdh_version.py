#!/usr/bin/env python3
# test_gdh_version.py
#
# Tests for the GDH fork version scheme (scripts/gdh_version.py): resolution,
# the release cut on main (`next`, `release`), the PR guard (`check-pr`), and
# the CMake block in CMakeLists.txt that has to agree with the resolver.
#
# The version rule is expressed twice on purpose: CMake derives APP_VERSION at
# configure time without needing Python, and scripts/gdh_version.py drives the
# tooling. test_cmake_block_matches_script is what keeps the two from drifting.
#
# Run with:
#   python3 -m unittest discover -s ci/gdh -t .

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

    def commit(self, subject: str, body: str = "", path: str = "log.txt") -> str:
        """Commit a change to `path`: log.txt ships nothing, src/... does."""
        # Touch a file so every commit is non-empty and ordering is stable.
        marker = self.root / path
        marker.parent.mkdir(parents=True, exist_ok=True)
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

    def show(self, *args: str) -> str:
        result = run_script("show", *args, script=self.script)
        if result.returncode != 0:
            raise AssertionError(f"show failed: {result.stderr}")
        return result.stdout.strip()

    def resolution(self) -> dict:
        return json.loads(self.show("--json"))

    def next_release(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("next", *args, script=self.script)

    def plan(self, *args: str) -> dict:
        result = self.next_release("--json", *args)
        if result.returncode not in (0, 2):
            raise AssertionError(f"next failed: {result.stderr}")
        return json.loads(result.stdout)

    def release(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("release", *args, script=self.script)

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

    The file is exact on the first-parent commit that introduced it, so a
    checkout that has not fetched the release tag still resolves the release
    (gdh-release.yml commits the file and tags the same commit).
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


def released_repo(stack: unittest.TestCase) -> FakeRepo:
    """A repo whose last release is v3.2.8-gdh-1.2, as the release workflow leaves it."""
    repo = FakeRepo(stack)
    repo.commit_version("3.2.8-gdh-1.2")
    repo.tag("v3.2.8-gdh-1.2")
    return repo


class TestNextRelease(unittest.TestCase):
    """`next`: what changed since the last release decides whether, and how big."""

    def test_a_shipped_fix_is_a_minor_release(self):
        """PR #75's case: a fix to tbc-analyse used to release nothing."""
        repo = released_repo(self)
        repo.commit("fix(tbc-analyse): ask before quitting mid-export", path="src/tbc-analyse/x.cpp")
        plan = repo.plan()
        self.assertEqual((plan["level"], plan["version"]), ("minor", "3.2.8-gdh-1.3"))

    def test_any_commit_type_that_ships_releases(self):
        for subject in ("refactor: tidy", "perf: faster", "chore: rename", "build: flags"):
            with self.subTest(subject=subject):
                repo = released_repo(self)
                repo.commit(subject, path="src/lib/y.cpp")
                self.assertEqual(repo.plan()["version"], "3.2.8-gdh-1.3")

    def test_nothing_shipped_means_no_release(self):
        repo = released_repo(self)
        repo.commit("docs: glossary", path="docs/glossary.md")
        repo.commit("ci: tweak", path=".github/workflows/x.yml")
        result = repo.next_release()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIsNone(repo.plan()["version"])

    def test_every_shipped_path_counts(self):
        for path in ("src/a.cpp", "scripts/tool.sh", "CMakeLists.txt", "CMakePresets.json",
                     "flake.nix", "flake.lock"):
            with self.subTest(path=path):
                repo = released_repo(self)
                repo.commit("fix: change", path=path)
                self.assertEqual(repo.plan()["level"], "minor")

    def test_a_bang_is_a_major_release(self):
        repo = released_repo(self)
        repo.commit("feat(cli)!: drop --old", path="src/cli.cpp")
        self.assertEqual(repo.plan()["version"], "3.2.8-gdh-2.0")

    def test_a_breaking_change_trailer_is_a_major_release(self):
        repo = released_repo(self)
        repo.commit("fix: new format", body="BREAKING CHANGE: old files no longer load",
                    path="src/io.cpp")
        self.assertEqual(repo.plan()["level"], "major")

    def test_a_breaking_marker_on_a_docs_commit_does_not_count(self):
        repo = released_repo(self)
        repo.commit("fix: small", path="src/a.cpp")
        repo.commit("docs!: rewrite the guide", path="docs/guide.md")
        self.assertEqual(repo.plan()["level"], "minor")

    def test_two_merged_prs_make_one_release(self):
        """Two quick merges: one release ships both."""
        repo = released_repo(self)
        repo.pr_merge(lambda r: r.commit("fix: one", path="src/a.cpp"))
        repo.pr_merge(lambda r: r.commit("fix: two", path="src/b.cpp"),
                      subject="Merge pull request #2 from GDH-Technologies/side")
        plan = repo.plan()
        self.assertEqual(plan["version"], "3.2.8-gdh-1.3")
        self.assertEqual([c["subject"] for c in plan["commits"]], ["fix: one", "fix: two"])

    def test_an_upstream_sync_restarts_the_series(self):
        repo = released_repo(self)
        repo.set_upstream("3.2.9")
        repo.commit("chore: merge upstream v3.2.9", path="src/up.cpp")
        self.assertEqual(repo.plan()["version"], "3.2.9-gdh-1.0")

    def test_a_forced_level_releases_with_nothing_shipped(self):
        repo = released_repo(self)
        self.assertEqual(repo.plan("--level", "minor")["version"], "3.2.8-gdh-1.3")
        self.assertEqual(repo.plan("--level", "major")["version"], "3.2.8-gdh-2.0")


class TestRelease(unittest.TestCase):
    """`release`: one commit of .gdh-version and its tag, pushed together or not at all."""

    def test_release_commits_the_file_and_tags_that_commit(self):
        repo = released_repo(self)
        repo.commit("fix: the fix", path="src/a.cpp")
        result = repo.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(repo.read_version_file(), "3.2.8-gdh-1.3")
        head = repo.git("rev-parse", "HEAD")
        self.assertEqual(repo.git("rev-parse", "v3.2.8-gdh-1.3^{commit}"), head)
        self.assertEqual(repo.git("log", "-1", "--format=%s"), "chore(release): 3.2.8-gdh-1.3")
        self.assertIn("fix: the fix", repo.git("log", "-1", "--format=%b"))
        self.assertEqual(repo.show(), "3.2.8-gdh-1.3")

    def test_after_a_release_there_is_nothing_to_release(self):
        """The release commit only touches .gdh-version, which does not ship."""
        repo = released_repo(self)
        repo.commit("fix: the fix", path="src/a.cpp")
        repo.release()
        self.assertEqual(repo.release().returncode, 2)

    def test_release_with_nothing_shipped_changes_nothing(self):
        repo = released_repo(self)
        head = repo.git("rev-parse", "HEAD")
        self.assertEqual(repo.release().returncode, 2)
        self.assertEqual(repo.git("rev-parse", "HEAD"), head)

    def test_release_refuses_a_dirty_tree(self):
        repo = released_repo(self)
        repo.commit("fix: the fix", path="src/a.cpp")
        (repo.root / "src" / "a.cpp").write_text("uncommitted\n", encoding="utf-8")
        result = repo.release()
        self.assertEqual(result.returncode, 1)
        self.assertIn("dirty", result.stderr)

    def test_push_sends_the_commit_and_tag_atomically(self):
        repo = released_repo(self)
        remote = repo.add_bare_remote(self)
        repo.git("push", "-q", "origin", "main", "--tags")
        repo.commit("fix: the fix", path="src/a.cpp")
        result = repo.release("--push")
        self.assertEqual(result.returncode, 0, result.stderr)
        heads = subprocess.run(["git", "-C", str(remote), "rev-parse", "main", "v3.2.8-gdh-1.3^{commit}"],
                               capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(heads, [repo.git("rev-parse", "HEAD")] * 2)

    def test_a_rejected_push_leaves_origin_untouched(self):
        """main moved under the run: neither the commit nor the tag lands."""
        repo = released_repo(self)
        remote = repo.add_bare_remote(self)
        repo.git("push", "-q", "origin", "main", "--tags")
        other = Path(tempfile.mkdtemp(prefix="gdh-other-"))
        self.addCleanup(shutil.rmtree, other, ignore_errors=True)
        subprocess.run(["git", "clone", "-q", "-b", "main", str(remote), str(other)], check=True)
        for args in (("config", "user.email", "o@example.invalid"), ("config", "user.name", "O"),
                     ("commit", "-q", "--allow-empty", "-m", "fix: someone else"),
                     ("push", "-q", "origin", "main")):
            subprocess.run(["git", "-C", str(other), *args], check=True)
        repo.commit("fix: the fix", path="src/a.cpp")
        result = repo.release("--push")
        self.assertEqual(result.returncode, 1)
        tags = subprocess.run(["git", "-C", str(remote), "tag", "--list", "v3.2.8-gdh-1.3"],
                              capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(tags, "")


class TestCheckPr(unittest.TestCase):
    """PRs never touch .gdh-version: the release workflow owns it."""

    def test_a_pr_that_leaves_the_file_alone_passes(self):
        repo = released_repo(self)
        repo.git("checkout", "-q", "-b", "pr")
        repo.commit("fix: work", path="src/a.cpp")
        self.assertEqual(repo.check_pr("--base", "main").returncode, 0)

    def test_a_pr_that_changes_the_file_fails(self):
        repo = released_repo(self)
        repo.git("checkout", "-q", "-b", "pr")
        repo.commit_version("3.2.8-gdh-1.3")
        result = repo.check_pr("--base", "main")
        self.assertEqual(result.returncode, 1)
        self.assertIn("only the release workflow writes it", result.stderr)


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
set(APP_BUILD "unknown" CACHE STRING "How this build was made: nix, git or unknown")
set(APP_SOURCE_ID "unknown" CACHE STRING "Source tree id (src-<hash> or tree-<hash>), not a commit")
{block}
message(STATUS "APP_VERSION=${{APP_VERSION}}")
message(STATUS "APP_BUILD=${{APP_BUILD}}")
message(STATUS "APP_SOURCE_ID=${{APP_SOURCE_ID}}")
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
        self.assertEqual(values["APP_BUILD"], "git")
        self.assertEqual(values["APP_SOURCE_ID"], f"tree-{tree}")

    def test_an_explicit_build_identity_wins(self):
        repo, build = self.project()
        values = self.configure(repo, build, "-DAPP_BUILD=nix", "-DAPP_SOURCE_ID=src-abc")
        self.assertEqual(values["APP_SOURCE_ID"], "src-abc")
        self.assertEqual(values["APP_BUILD"], "nix")

    def test_the_stub_mirrors_the_real_placeholders(self):
        # The block only fills in the identity while both still hold the
        # top-level CMakeLists.txt's placeholders, so the stub must match them.
        text = CMAKELISTS.read_text(encoding="utf-8")
        for line in STUB_PROJECT.splitlines()[2:4]:
            self.assertIn(line, text)


if __name__ == "__main__":
    unittest.main()
