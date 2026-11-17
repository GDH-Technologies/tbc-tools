#!/usr/bin/env python3
# check_ci.py
#
# The GDH fork's CI policy checks. Upstream's ci/check_ci_contracts.py is left
# exactly as upstream ships it; these are the fork's own rules, for the fork's
# own workflows. Each rule exists for a reason stated beside it, and checks
# behaviour-relevant structure -- never prose, step names or version pins.
#
# Run with:  python3 ci/gdh/check_ci.py      (the guardrails workflow does)
# Tests:     python3 -m unittest discover -s ci/gdh -t .

from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = Path(".github/workflows")

# The fork's workflows. Everything else in .github/workflows is upstream's, kept
# byte-identical to harrypm/tbc-tools and disabled in this repository's settings.
FORK_WORKFLOWS = (
    "gdh-release.yml",
    "self-hosted-guardrails.yml",
    "self-hosted-linux.yml",
    "self-hosted-macos.yml",
    "self-hosted-windows.yml",
    "self-hosted-deploy.yml",
)
PLATFORM_WORKFLOWS = ("self-hosted-linux.yml", "self-hosted-macos.yml", "self-hosted-windows.yml")
RELEASE_WORKFLOW = "gdh-release.yml"

# Scripts that run on air0, whose /bin/bash is 3.2. `bash -n` on wm runs bash
# 5, which accepts all of these, so they are refused by name.
AIR0_SCRIPTS = ("ci/gdh/smoke_installed.sh",)
BASH4_ONLY = ("declare -A", "mapfile", "readarray", ",,}", "^^}")

FORK_PR_GUARD = "github.event.pull_request.head.repo.full_name == github.repository"


def shipped_paths(root: Path) -> list[str]:
    """SHIPPED_PATHS from scripts/gdh_version.py, as workflow `paths` globs."""
    spec = importlib.util.spec_from_file_location("gdh_version", root / "scripts/gdh_version.py")
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {root / 'scripts/gdh_version.py'}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return [p + "**" if p.endswith("/") else p for p in module.SHIPPED_PATHS]


def trigger_paths(text: str, event: str) -> list[str] | None:
    """The `paths` list under `on.<event>`, or None when the event has none.

    A deliberately small reader for the layout these workflows use:
    two-space indents, one quoted path per `- "..."` line.
    """
    lines = text.splitlines()
    try:
        start = lines.index(f"  {event}:")
    except ValueError:
        return None
    paths: list[str] | None = None
    for line in lines[start + 1:]:
        if line.strip() and not line.startswith("    "):
            break  # the next event, or the end of `on:`
        if line.strip() == "paths:":
            paths = []
        elif paths is not None:
            match = re.match(r'^\s+- "([^"]+)"\s*$', line)
            if match:
                paths.append(match.group(1))
            elif line.strip() and not line.strip().startswith("#"):
                break
    return paths


def has_trigger(text: str, event: str) -> bool:
    return re.search(rf"^  {re.escape(event)}:", text, re.MULTILINE) is not None


def check(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    texts: dict[str, str] = {}
    for name in FORK_WORKFLOWS:
        path = root / WORKFLOWS / name
        if not path.is_file():
            errors.append(f"{name}: missing")
            continue
        texts[name] = path.read_text(encoding="utf-8")

    for name, text in texts.items():
        # The runners are workstations and the repository is public: nothing
        # that can run a fork's code with this repository's context.
        for event in ("pull_request_target", "workflow_run"):
            if has_trigger(text, event):
                errors.append(f"{name}: `{event}` trigger is forbidden")

        # Self-hosted only: GitHub-hosted minutes are billable.
        for line in re.findall(r"^\s+runs-on:.*$", text, re.MULTILINE):
            if "self-hosted" not in line:
                errors.append(f"{name}: hosted runner ({line.strip()})")

        # No secrets: the fork's workflows need nothing beyond GITHUB_TOKEN.
        if re.search(r"\$\{\{\s*secrets\.", text):
            errors.append(f"{name}: references a secret")

        # One write token in the repository: the release job's contents: write.
        writes = re.findall(r"^\s+[a-z-]+:\s*write\s*$", text, re.MULTILINE)
        if name != RELEASE_WORKFLOW and writes:
            errors.append(f"{name}: write permission ({writes[0].strip()}); only {RELEASE_WORKFLOW} may write")

        # Only the release workflow runs on a push: everything else is a PR
        # check, a call, or a dispatch. (A push trigger elsewhere is how three
        # no-op runs per merge crept in.)
        if name != RELEASE_WORKFLOW and has_trigger(text, "push"):
            errors.append(f"{name}: `push` trigger; only {RELEASE_WORKFLOW} runs on push")

        # A PR from a fork must never run on a workstation runner.
        if has_trigger(text, "pull_request"):
            jobs = len(re.findall(r"^\s+runs-on:", text, re.MULTILINE))
            guards = text.count(FORK_PR_GUARD)
            if guards < jobs:
                errors.append(f"{name}: {jobs} self-hosted job(s) but {guards} fork-PR guard(s)")

    # One path list: what a PR is tested for is exactly what releases on merge.
    shipped = shipped_paths(root)
    if RELEASE_WORKFLOW in texts:
        release_paths = trigger_paths(texts[RELEASE_WORKFLOW], "push")
        if sorted(release_paths or []) != sorted(shipped):
            errors.append(f"{RELEASE_WORKFLOW}: push paths {release_paths} != SHIPPED_PATHS {shipped}")
    for name in PLATFORM_WORKFLOWS:
        if name in texts:
            expected = sorted(shipped + [f".github/workflows/{name}"])
            got = trigger_paths(texts[name], "pull_request")
            if sorted(got or []) != expected:
                errors.append(f"{name}: pull_request paths {got} != SHIPPED_PATHS + itself {expected}")

    # Guardrails always run: unfiltered, so no PR can skip them.
    guardrails = texts.get("self-hosted-guardrails.yml", "")
    if guardrails and trigger_paths(guardrails, "pull_request") is not None:
        errors.append("self-hosted-guardrails.yml: pull_request must not be path-filtered")

    for script in AIR0_SCRIPTS:
        path = root / script
        if not path.is_file():
            errors.append(f"{script}: missing")
            continue
        text = path.read_text(encoding="utf-8")
        for snippet in BASH4_ONLY:
            if snippet in text:
                errors.append(f"{script}: `{snippet}` is not bash 3.2 (air0)")

    for script in sorted((root / "ci/gdh").glob("*.sh")):
        result = subprocess.run(["bash", "-n", str(script)], capture_output=True, text=True)
        if result.returncode != 0:
            errors.append(f"{script.relative_to(root)}: bash -n: {result.stderr.strip()}")

    return errors


def main() -> int:
    errors = check()
    for error in errors:
        print(f"check_ci: {error}", file=sys.stderr)
    if errors:
        return 1
    print("GDH CI checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
