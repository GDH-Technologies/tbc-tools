#!/usr/bin/env bash
# Make a staged .app self-contained: copy every /nix/store dylib and framework
# it loads into Contents/Frameworks, rewrite the load commands to @rpath, give
# every Mach-O the bundle's rpaths, and drop /nix/store rpaths.
#
# Usage: bash ci/macos_bundle_dependencies.sh dist/tbc-tools.app
#
# Same result as the inline step it replaced, with far fewer processes: each
# file gets ONE install_name_tool call carrying all its edits (the old step
# made one per rpath and per dependency), the dependency naming is done with
# parameter expansion instead of sed subshells, and the work queue is walked
# by index against an in-memory seen list instead of a grep over a growing
# file per entry. If a combined call is rejected, its edits are retried one at
# a time, which is what the old step did for every edit.
#
# bash 3.2 safe (macOS /bin/bash): no associative arrays, no mapfile.
set -uo pipefail

APP="${1:?usage: $0 path/to/App.app}"
FW_DIR="$APP/Contents/Frameworks"
PLUGINS_DIR="$APP/Contents/PlugIns"
WANT_RPATHS=("@executable_path/../Frameworks" "@loader_path/../Frameworks" "@loader_path/../../Frameworks")

mkdir -p "$FW_DIR" "$PLUGINS_DIR"
chmod -R u+w "$APP/Contents" 2>/dev/null || true

is_macho() {
  file "$1" 2>/dev/null | grep -q "Mach-O"
}

# Sets DEP_TARGET (the copy inside the bundle) and DEP_REF (its @rpath name).
copy_dep() {
  local dep="$1" framework_root framework_name version bin_name rest store_entry unique_name
  if [[ "$dep" == *".framework/"* ]]; then
    framework_root="${dep%.framework/*}.framework"
    framework_name="${framework_root##*/}"
    if [[ "$dep" == *".framework/Versions/"* ]]; then
      version="${dep##*.framework/Versions/}"
      version="${version%%/*}"
    else
      version="$dep"
    fi
    bin_name="${dep##*/}"
    DEP_TARGET="$FW_DIR/$framework_name/Versions/$version/$bin_name"
    DEP_REF="@rpath/$framework_name/Versions/$version/$bin_name"
    if [ ! -f "$DEP_TARGET" ]; then
      rsync -a "$framework_root" "$FW_DIR/"
      chmod -R u+w "$FW_DIR/$framework_name" || true
    fi
  else
    rest="${dep#/nix/store/}"
    store_entry="${rest%%/*}"
    unique_name="${store_entry%%-*}_${dep##*/}"
    DEP_TARGET="$FW_DIR/$unique_name"
    DEP_REF="@rpath/$unique_name"
    if [ ! -f "$DEP_TARGET" ]; then
      cp "$dep" "$DEP_TARGET" 2>/dev/null || true
      chmod u+w "$DEP_TARGET" 2>/dev/null || true
    fi
  fi
}

apply_edits() {
  local file="$1"
  shift
  [ "$#" -gt 0 ] || return 0
  install_name_tool "$@" "$file" 2>/dev/null && return 0
  # One rejected edit rejects the whole call; fall back to one call per edit.
  while [ "$#" -gt 0 ]; do
    case "$1" in
      -change) install_name_tool -change "$2" "$3" "$file" 2>/dev/null || true; shift 3 ;;
      *)       install_name_tool "$1" "$2" "$file" 2>/dev/null || true; shift 2 ;;
    esac
  done
}

process_file() {
  local file="$1" existing rp dep
  local -a edits=()
  is_macho "$file" || return 0
  chmod u+w "$file" 2>/dev/null || true

  existing="$(otool -l "$file" 2>/dev/null | awk '/LC_RPATH/{getline;getline;print $2}')"
  for rp in "${WANT_RPATHS[@]}"; do
    case $'\n'"$existing"$'\n' in
      *$'\n'"$rp"$'\n'*) ;;
      *) edits+=(-add_rpath "$rp") ;;
    esac
  done
  while IFS= read -r rp; do
    case "$rp" in /nix/store/*) edits+=(-delete_rpath "$rp") ;; esac
  done <<< "$existing"

  while IFS= read -r dep; do
    case "$dep" in
      # Only Nix store paths. Mixing host (e.g. Homebrew) dylibs with Nix ones
      # causes ABI mismatches such as libidn2 wanting GNU libiconv symbols.
      /nix/store/*)
        copy_dep "$dep"
        edits+=(-change "$dep" "$DEP_REF")
        QUEUE+=("$DEP_TARGET")
        ;;
    esac
  done < <(otool -L "$file" 2>/dev/null | tail -n +2 | awk '{print $1}')

  apply_edits "$file" ${edits[@]+"${edits[@]}"}
}

QUEUE=()
while IFS= read -r -d '' f; do QUEUE+=("$f"); done < <(find "$APP/Contents/MacOS" "$FW_DIR" "$PLUGINS_DIR" -type f -print0)

SEEN=$'\n'
processed=0
i=0
while [ "$i" -lt "${#QUEUE[@]}" ]; do
  file="${QUEUE[$i]}"
  i=$((i + 1))
  case "$SEEN" in *$'\n'"$file"$'\n'*) continue ;; esac
  SEEN="$SEEN$file"$'\n'
  process_file "$file"
  processed=$((processed + 1))
done

ids=0
while IFS= read -r -d '' lib; do
  is_macho "$lib" || continue
  chmod u+w "$lib" 2>/dev/null || true
  rel="${lib#"$FW_DIR"/}"
  if [[ "$rel" == *.framework/* ]]; then
    install_name_tool -id "@rpath/$rel" "$lib" 2>/dev/null || true
  else
    install_name_tool -id "@rpath/${lib##*/}" "$lib" 2>/dev/null || true
  fi
  ids=$((ids + 1))
done < <(find "$FW_DIR" -type f -print0)

echo "Bundled dependencies: processed $processed Mach-O candidates, set $ids install ids in $FW_DIR"
