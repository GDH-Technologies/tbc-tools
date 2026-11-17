#!/usr/bin/env bash
# Smoke-test an installed tbc-tools build: smoke_installed.sh <store-path>
#
# The one check every host's install gets (self-hosted-deploy.yml): wm and air0
# run it locally, the Linux fleet over `ssh <host> bash -s -- <store-path>`.
# Bash 3.2 clean, for air0.
#
# It reads the binaries from the store path just installed, never from PATH:
# a runner service's PATH need not include the nix profile, and that says
# nothing about the install. PATH is only reported, as a warning.
set -euo pipefail

STORE="${1:?usage: smoke_installed.sh <store-path>}"
HOST="$(hostname)"

case "$STORE" in
  /nix/store/*-tbc-tools-*) ;;
  *) echo "$HOST: not a tbc-tools store path: '$STORE'" >&2; exit 1 ;;
esac

# The GUI: bin/tbc-analyse on Linux, a bundle on macOS.
GUI=""
for candidate in "$STORE/bin/tbc-analyse" "$STORE/bin/tbc-analyse.app/Contents/MacOS/tbc-analyse"; do
  if [ -x "$candidate" ]; then GUI="$candidate"; break; fi
done
if [ -z "$GUI" ]; then
  echo "$HOST: no tbc-analyse in $STORE/bin (plain or .app layout)" >&2
  ls -la "$STORE/bin" >&2 || true
  exit 1
fi

"$STORE/bin/tbc-video-export" --version

# tbc-video-export's MKV display-dimension normalization falls back to the
# mkvmerge the package ships in libexec; without one it silently skips it. Run
# under the host's own locale, as the workers do.
"$STORE/libexec/tbc-video-export/mkvmerge" --version

# The post-decode pipeline drives the headless aligner. With no arguments it
# prints its usage and exits 2, which proves the headless entry point shipped
# without touching any media.
set +e
QT_QPA_PLATFORM=offscreen "$STORE/bin/tbc-audio-align" --headless </dev/null >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" -ne 2 ]; then
  echo "$HOST: tbc-audio-align --headless with no arguments exited $rc, expected 2 (usage)" >&2
  exit 1
fi

if ! command -v tbc-video-export >/dev/null 2>&1; then
  echo "::warning::$HOST: tbc-tools is installed but not on PATH for this session"
fi
echo "$HOST: smoke test passed for $STORE (GUI at $GUI)"
