#!/usr/bin/env python3
# test_check_ci.py
#
# Tests that each rule in ci/gdh/check_ci.py actually fires. Every test copies
# the real repository's fork CI files into a scratch tree, breaks exactly one
# thing, and expects exactly that rule to report it -- so a rule that stopped
# checking anything fails here.
#
# Run with:  python3 -m unittest discover -s ci/gdh -t .

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from ci.gdh import check_ci

REPO = check_ci.ROOT


class Tree:
    """A scratch copy of the files check_ci reads."""

    def __init__(self, test: unittest.TestCase) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="gdh-check-ci-"))
        test.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        for rel in (
            *(f".github/workflows/{name}" for name in check_ci.FORK_WORKFLOWS),
            "scripts/gdh_version.py",
            *(str(p.relative_to(REPO)) for p in (REPO / "ci/gdh").glob("*.sh")),
        ):
            dest = self.root / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, dest)

    def edit(self, rel: str, old: str, new: str) -> None:
        path = self.root / rel
        text = path.read_text(encoding="utf-8")
        assert old in text, f"{old!r} not in {rel}"
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def errors(self) -> list[str]:
        return check_ci.check(self.root)


LINUX = ".github/workflows/self-hosted-linux.yml"
RELEASE = ".github/workflows/gdh-release.yml"
DEPLOY = ".github/workflows/self-hosted-deploy.yml"
GUARDRAILS = ".github/workflows/self-hosted-guardrails.yml"


class TestCheckCi(unittest.TestCase):
    def assertOnly(self, errors: list[str], fragment: str) -> None:
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(fragment, errors[0])

    def test_the_repository_passes(self):
        self.assertEqual(check_ci.check(), [])

    def test_a_scratch_copy_passes(self):
        self.assertEqual(Tree(self).errors(), [])

    def test_pull_request_target_is_refused(self):
        tree = Tree(self)
        tree.edit(LINUX, "  workflow_dispatch:\n", "  workflow_dispatch:\n  pull_request_target:\n")
        self.assertOnly(tree.errors(), "pull_request_target")

    def test_a_hosted_runner_is_refused(self):
        tree = Tree(self)
        tree.edit(LINUX, "runs-on: [self-hosted, wm-test]", "runs-on: ubuntu-latest")
        self.assertOnly(tree.errors(), "hosted runner")

    def test_a_secret_is_refused(self):
        tree = Tree(self)
        tree.edit(DEPLOY, "GH_TOKEN: ${{ github.token }}", "GH_TOKEN: ${{ secrets.PAT }}")
        self.assertOnly(tree.errors(), "references a secret")

    def test_write_permission_outside_the_release_is_refused(self):
        tree = Tree(self)
        tree.edit(DEPLOY, "permissions:\n  contents: read", "permissions:\n  contents: write")
        self.assertOnly(tree.errors(), "write permission")

    def test_a_push_trigger_outside_the_release_is_refused(self):
        tree = Tree(self)
        tree.edit(GUARDRAILS, "  workflow_dispatch:\n", "  workflow_dispatch:\n  push:\n")
        self.assertOnly(tree.errors(), "`push` trigger")

    def test_a_missing_fork_pr_guard_is_refused(self):
        tree = Tree(self)
        tree.edit(LINUX, check_ci.FORK_PR_GUARD, "true")
        self.assertOnly(tree.errors(), "fork-PR guard")

    def test_a_platform_path_list_that_drifts_is_refused(self):
        tree = Tree(self)
        tree.edit(LINUX, '      - "vcpkg.json"\n', "")
        self.assertOnly(tree.errors(), "pull_request paths")

    def test_a_release_path_list_that_drifts_is_refused(self):
        tree = Tree(self)
        tree.edit(RELEASE, '      - "flake.lock"\n', '      - "flake.lock"\n      - "docs/**"\n')
        self.assertOnly(tree.errors(), "push paths")

    def test_shipped_paths_drive_both_lists(self):
        """Adding a shipped path without adding it to the workflows fails both."""
        tree = Tree(self)
        tree.edit("scripts/gdh_version.py", '    "flake.lock",\n', '    "flake.lock",\n    "extra/",\n')
        errors = tree.errors()
        self.assertEqual(len(errors), 1 + len(check_ci.PLATFORM_WORKFLOWS), errors)

    def test_filtering_the_guardrails_is_refused(self):
        tree = Tree(self)
        tree.edit(GUARDRAILS, "  pull_request:\n", '  pull_request:\n    paths:\n      - "src/**"\n')
        self.assertOnly(tree.errors(), "must not be path-filtered")

    def test_bash4_in_an_air0_script_is_refused(self):
        tree = Tree(self)
        tree.edit("ci/gdh/smoke_installed.sh", "set -euo pipefail\n", "set -euo pipefail\ndeclare -A seen\n")
        self.assertOnly(tree.errors(), "bash 3.2")

    def test_a_syntax_error_in_a_script_is_refused(self):
        tree = Tree(self)
        tree.edit("ci/gdh/deploy_fleet_host.sh", "set -euo pipefail\n", "set -euo pipefail\nif then\n")
        self.assertOnly(tree.errors(), "bash -n")

    def test_a_missing_workflow_is_reported(self):
        tree = Tree(self)
        (tree.root / DEPLOY).unlink()
        self.assertOnly(tree.errors(), "missing")


if __name__ == "__main__":
    unittest.main()
