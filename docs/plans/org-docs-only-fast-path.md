# Plan: an org-wide fast path for docs-only changes

Status: not started. Written 2026-09-28 as a starting point for later planning. Everything
here is a suggestion with its reasoning, not a decision.

## Goal

A pull request that only touches documentation should finish its checks in seconds and
merge without waking any build runner. This should hold in every GDH-Technologies
repository, not just tbc-tools, and a docs PR must never be stuck waiting on a check that
was skipped.

## Where tbc-tools already is (as of 0491e617)

tbc-tools already has this fast path; it needs nothing new. For a PR that touches only docs
(`docs/`, `*.md`, `notes/`, `dev-notes/`, `development-logs/`, `LICENSE`, `.github/` files
other than `self-hosted-*.yml`, `ci/tests/**`, `ci/check_ci_contracts.py`):

| Workflow | Docs-only PR | Docs-only merge to main |
| --- | --- | --- |
| `self-hosted-{linux,macos,windows}.yml` | not triggered (`paths:` filter) | not triggered |
| `self-hosted-deploy.yml` | n/a | not triggered (`paths:` filter) |
| `self-hosted-guardrails.yml` (wm-light) | about 15 s | about 15 s |
| `gdh-version-tag.yml` (wm-light) | n/a | about 10-25 s, and it tags nothing |
| hosted `build_*_tools.yml`, `tests.yml`, `release.yml` | dispatch, reusable or tag only | same |

The flake's `tbcSrc` also leaves out docs, so even a mixed PR's doc edits never change the
Nix derivation.

**What makes this safe:**
- The `main` ruleset only blocks deletion and force-push. It has no required status checks,
  so a platform workflow that never starts can't leave a PR waiting.
- AGENTS.md makes it a hard rule that `self-hosted-guardrails.yml` stays **unfiltered** (it
  triggers on `push` and `pull_request` with no `paths:`). It is therefore the only workflow
  that may ever be a required check. The workflow's header comment gives the reasons.

**One small inefficiency.** Guardrails runs on both `push` (every branch) and `pull_request`,
so each push to a PR branch runs it twice (#52 showed two "CI guardrails" checks). The
workflow's concurrency comment shows that both events were kept deliberately.
- Restricting `push` to `branches: [main]` would halve the runs. A branch pushed with no PR
  open would then get no guardrails run at all.
- Check that change against the AGENTS.md rule and `ci/check_ci_contracts.py` first. The
  rule forbids a `paths:` filter; it says nothing explicit about `branches:`.
- It saves about 15 s of wm-light per push, so it is optional.

## GitHub has no org-wide "docs-only mode"

Each workflow decides for itself whether to run. The building blocks:

1. **Path filters in each repository** (`paths:` / `paths-ignore:` on `pull_request` and
   `push`). This is what tbc-tools does. They must be written into each repository's
   workflows.
2. **One always-running gate job as the only required check.**
   - **The trap:** a required check whose workflow is path-filtered never reports on a
     docs-only PR, so the PR waits forever ("Expected - waiting for status").
   - **The fix:** a small job that always runs. It works out which paths changed and passes
     straight away when they are all docs. That job is the required check, and the heavy
     jobs depend on its output (`needs:` + `if:`).
   - Build the change detection with `dorny/paths-filter`, pinned to a commit SHA, or with
     plain `git diff --name-only "$BASE"...HEAD`.
   - Avoid `tj-actions/changed-files`: it was compromised in a supply-chain attack in March
     2025.
   - In tbc-tools, guardrails already plays the gate's role (always runs, cheap). Other
     repositories may need a dedicated gate job.
3. **Share it across the org.**
   - Put a reusable workflow (`on: workflow_call`) for the gate in the org's `.github`
     repository, so each repository needs only a few lines to call it.
   - Workflow templates in the same repository give new repositories that starting point.
   - Org rulesets can require a named check or workflow across many repositories. Check in
     the org settings which ruleset features the current plan (Team) includes before
     designing around them.
4. **`[skip ci]`** in a commit or merge title skips every `push` and `pull_request` workflow
   run for that commit. It's a manual per-merge escape hatch, not a process. In tbc-tools it
   would also skip `gdh-version-tag.yml`, and it saves only about 40 s.

## Suggested next steps

1. **Survey the active repositories, read-only:** digitization-toolkit, capture-node,
   MISRC-GUI, vhs-decode, gdhvc-orchestrator. For each, record:
   - which workflows a docs-only PR triggers;
   - which runners those workflows occupy, and for how long;
   - whether any ruleset or branch protection requires status checks, which would hit the
     trap above.
2. **Decide per repository:** path filters alone (the tbc-tools model, when nothing is a
   required check), or path filters plus a gate job (when required checks are wanted).
3. **If more than one repository needs the gate,** write it once as a reusable workflow in
   the org `.github` repository and adopt it repository by repository, one PR each.
4. **In tbc-tools,** optionally make the guardrails `branches:` change above, after the
   contract check.
