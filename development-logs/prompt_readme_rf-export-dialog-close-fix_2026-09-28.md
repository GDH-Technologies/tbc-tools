# Prompt Readme — RF export UI debugging + save-all dialog close fix
Date: 2026-09-28
Worktree: /tmp/tbc-wt-rf-export (branch feature/rf-segment-export)
Repo pair: tbc-tools (this worktree) + FLAC-Chop /home/harry/FLAC-Chop (branch feature/cli-gui-loading, child agent "flac-chop-cli" run 01a0e28c-f205-7c7b-9b07-2f8de91fe136)

## User input
- "Save all view modes window can't be closed and there is no RF export option"
- Clarification answers (real-world confirmation):
  - Launched tbc-analyse from the desktop menu/launcher
  - "Also export source RF segment (FLAC-Chop)" checkbox NOT visible in the save-all dialog
  - "Export source RF segment for frame..." NOT present in the File menu
  - X (window close) button on the dialog does nothing

## Diagnosis commands + hard data
1. Launcher: `~/.nix-profile/share/applications/tbc-analyse.desktop` → `Exec=tbc-analyse` → resolves to `~/.nix-profile/bin/tbc-analyse` (nix wrapper, 15920 B) → execs real binary via readlink -f.
2. Feature markers in profile binary (`strings ~/.nix-profile/bin/.tbc-analyse-wrapped | grep -c ...`):
   - "Export source RF segment for frame" → 2 (present)
   - "Also export source RF segment" → 1 (present)
3. Two tbc-tools-3.2.9 store paths found:
   - `3j0mcpb12ngy33crd65gpg3l7dp7rjmh-tbc-tools-3.2.9` — markers present (feature build; profile pointed here before today's rebuild)
   - `pdkxi9pwlcdhaa7wvhz75fpxbx67p69s-tbc-tools-3.2.9` — markers 0 (stale pre-feature build)
4. No other installs: /usr/local/bin, /usr/bin, /opt, /snap all empty of tbc-analyse. `nix profile list` → 3j0mcp… (pre-rebuild).
- Conclusion: the reported "missing RF export option" session was running an older binary (launched before the profile swap / old generation). Current profile binary has the features. Relaunch needed.

## Code defect found (real, pre-existing)
- `src/tbc-analyse/mainwindow.cpp` `on_actionSave_all_modes_as_PNGs_triggered()`: exportModeDialog had only AcceptRole/ActionRole buttons (Everything/Image/Scopes/SNR Graphs) — no RejectRole. Qt's QMessageBox X-close/Escape click the escape (reject) button; with none set the close event is ignored → "X does nothing".
- RF export handler (`runRfSegmentExport`, probeRfSource) uses only standard warning boxes/QFileDialog — closes fine, no change needed.
- CI contracts (ci/check_ci_contracts.py): no references to this dialog — safe to edit.

## Fix applied (this worktree, uncommitted)
- Added `QAbstractButton *cancelButton = exportModeDialog.addButton(tr("Cancel"), QMessageBox::RejectRole);` after the SNR Graphs button.
- Early return on cancel: `if (!selectedExportModeButton || selectedExportModeButton == cancelButton) return;`
- Existing Escape QShortcut left in place (reject → null clickedButton → same early return).

## Build + tests
- `nix develop -c sh -c 'cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build'` → OK, `[25/25] Linking CXX executable bin/tbc-analyse`
- `nix develop -c ctest --test-dir build --output-on-failure` → 100% pass, 30/30 tests, 58.01 s
- Profile: `nix profile install .#` deduped ("already added") → had to `nix profile remove tbc-wt-rf-export` then install again.
- Profile now → `/nix/store/gq4mysk9fs6c7akcm4mp7ikxqb6lbhhr-tbc-tools-3.2.9` (dialog-fixed build), markers verified (2).

## FLAC-Chop child agent status (parallel)
- GUI pre-load (`--gui file --in/--out`) interactively confirmed by user earlier.
- New fix on feature/cli-gui-loading (gui/mainwindow.cpp only): Apply Template button now enabled for ANY loaded FLAC (was gated on is_rf), non-RF gets 5 blank ingest rows, status messages instead of silent no-ops. Rebuilt; GUI relaunched (PID 3063197) awaiting user confirmation; commit/push held until then.

## Pending
- User: fully close any running tbc-analyse, relaunch from desktop menu, confirm (a) File menu entry present, (b) checkbox present, (c) X/Cancel closes the dialog.
- Commit/push of the dialog fix after user confirmation (GH Actions must pass per project rules).
