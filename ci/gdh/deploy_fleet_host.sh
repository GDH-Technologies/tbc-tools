#!/usr/bin/env bash
# Install one Linux fleet host's tbc-tools from a build wm has already copied in.
#
# self-hosted-deploy.yml's deploy-fleet job runs this ON the target host, as
# `ssh <host> bash -s -- <sha> <store-path> < ci/gdh/deploy_fleet_host.sh`, right
# after `nix copy` has pushed wm's installed closure there. The flake builds from
# a content-filtered source (tbcSrc in flake.nix), so the github: flake at <sha>
# evaluates to exactly the store path wm installed. The profile operation is
# therefore a store hit, and --max-jobs 0 turns any divergence into a failure
# instead of a compile. cs0 and cs1 are capture servers with four weak cores;
# a surprise tbc-tools build there could cost a capture.
set -euo pipefail

SHA="${1:?usage: deploy_fleet_host.sh <sha> <store-path>}"
STORE="${2:?usage: deploy_fleet_host.sh <sha> <store-path>}"
FLAKE_URL="github:GDH-Technologies/tbc-tools"
HOST="$(hostname)"

if [ ! -d "$STORE" ]; then
  echo "Refusing to deploy on $HOST: $STORE is not in this store; the nix copy did not land." >&2
  exit 1
fi

# Nix renamed `profile install` to `profile add`; support both.
if nix profile add --help >/dev/null 2>&1; then ADD=add; else ADD=install; fi

profile_list() { nix profile list | sed -e 's/\x1b\[[0-9;]*m//g'; }

# The tbc-tools record's Original flake URL, or nothing. Scoped to the record:
# other packages' lines sort around it.
ORIGINAL="$(profile_list | awk '/^Name:[[:space:]]+tbc-tools$/ { found = 1; next }
                                found && /^Name:/ { exit }
                                found && /^Original flake URL:/ { print $4; exit }')"
HAS_ENTRY=false
if profile_list | grep -qE '^Name:[[:space:]]+tbc-tools$'; then HAS_ENTRY=true; fi

if [ "$HAS_ENTRY" = true ] && [ "$ORIGINAL" = "$FLAKE_URL" ]; then
  echo "Existing $FLAKE_URL entry on $HOST; upgrading."
  # --refresh: github: refs are served from the tarball cache (default TTL 1h)
  # and would otherwise resolve to a stale commit.
  nix profile upgrade --refresh --max-jobs 0 tbc-tools
else
  if [ "$HAS_ENTRY" = true ]; then
    # A bare store-path entry (cs0 before this deploy existed) or a git+file
    # one cannot follow main. Replace it with the github: flake.
    echo "Replacing $HOST's tbc-tools entry (original: '${ORIGINAL:-<store path, no flake>}') with $FLAKE_URL."
    nix profile remove tbc-tools
  else
    echo "No tbc-tools entry on $HOST; performing first install."
  fi
  nix profile "$ADD" --refresh --max-jobs 0 "$FLAKE_URL"
fi

PROFILE="$(mktemp)"
trap 'rm -f "$PROFILE"' EXIT
profile_list > "$PROFILE"
cat "$PROFILE"

# Same check as air0: a github: lock reads github:owner/repo/<sha>.
if ! grep -q "Locked flake URL:.*$SHA" "$PROFILE"; then
  echo "$HOST profile is not at $SHA. Locked flake URL reads:" >&2
  grep "Locked flake URL:" "$PROFILE" >&2 || true
  exit 1
fi

INSTALLED="$(awk '/^Name:[[:space:]]+tbc-tools$/ { found = 1; next }
                  found && /^Store paths:/ { print $3; exit }' "$PROFILE")"
if [ "$INSTALLED" != "$STORE" ]; then
  echo "Refusing to report success on $HOST: the profile resolved $INSTALLED, not wm's $STORE." >&2
  exit 1
fi

# The deploy then runs ci/gdh/smoke_installed.sh on this host.
echo "Installed $SHA on $HOST: $STORE"
