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

    def show(self, *args: str) -> str:
        result = run_script("show", *args, script=self.script)
        if result.returncode != 0:
            raise AssertionError(f"show failed: {result.stderr}")
        return result.stdout.strip()

    def resolution(self) -> dict:
        return json.loads(self.show("--json"))

    def propose(self, *args: str) -> subprocess.CompletedProcess:
        return run_script("propose", *args, script=self.script)


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


class TestCMakeParity(unittest.TestCase):
    """The CMake block and the script must resolve the same version."""

    def extract_block(self) -> str:
        text = CMAKELISTS.read_text(encoding="utf-8")
        start = text.index(BEGIN_MARKER)
        end = text.index(END_MARKER) + len(END_MARKER)
        return text[start:end]

    def test_markers_are_present_and_ordered(self):
        text = CMAKELISTS.read_text(encoding="utf-8")
        self.assertEqual(text.count(BEGIN_MARKER), 1)
        self.assertEqual(text.count(END_MARKER), 1)
        self.assertLess(text.index(BEGIN_MARKER), text.index(END_MARKER))

    @unittest.skipIf(shutil.which("cmake") is None, "cmake is not installed")
    def test_cmake_block_matches_script(self):
        block = self.extract_block()
        with tempfile.TemporaryDirectory(prefix="gdh-cmake-") as tmp:
            script = Path(tmp) / "resolve.cmake"
            script.write_text(
                f'set(CMAKE_SOURCE_DIR "{REPO_ROOT.as_posix()}")\n'
                f"{block}\n"
                'message(STATUS "APP_VERSION=${APP_VERSION}")\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        match = re.search(r"APP_VERSION=(\S+)", result.stderr + result.stdout)
        self.assertIsNotNone(match, f"no APP_VERSION in cmake output:\n{result.stderr}")

        expected = run_script("show").stdout.strip()
        self.assertEqual(match.group(1), expected)


if __name__ == "__main__":
    unittest.main()
