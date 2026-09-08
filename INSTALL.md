# Installation (Nix)

This project ships a Nix flake that provides both a build and a development environment. The simplest way to install is with Nix flakes.

## Prerequisites

- Nix with flakes enabled.
  - Ensure `experimental-features = nix-command flakes` is set in your Nix config.

## Install with Nix

### Install into your user profile

```bash
nix profile install .#
```

This installs the tools into your Nix profile (for example, `~/.nix-profile/bin`).

### Build without installing

```bash
nix build .#
```

The build output will be available under `./result`.

### Run from the build output

```bash
./result/bin/ld-analyse
```

Replace `ld-analyse` with any other tool from the suite.

## Notes

- The flake pins all required dependencies, including Qt6, FFmpeg, FFTW, SQLite, and OpenGL.
- Build metadata (branch/commit) is set by the flake during the build.

## Desktop integration (Linux)

The Linux build ships file associations for the decode toolchain: the
shared-mime-info packages that define the types, `ld-analyse.desktop`, the
application and mimetype icons, and `decode-desktop-sync`, which registers them.

| Family | Types | Opened by |
| --- | --- | --- |
| TBC | `.tbc` `.tbcy` `.tbcc` `.ytbc` `.ctbc` `.tbc.db` `.tbc.json` | `ld-analyse` |
| RF | video / hifi / combined captures, `.u8` and `.flac` | `decode-launcher`, `hifi-decode --gui` |
| ORC | `.orcprj` | `orc-gui` |

### Why `decode-desktop-sync` is a required step, not a convenience

A `nix profile` install registers **nothing** with XDG, because nix never runs
the cache builders. `~/.nix-profile/share/mime/` contains only `packages/` — no
`mime.cache`, no `globs2`; `share/applications/` has no `mimeinfo.cache`; and
`share/icons/hicolor` has no `index.theme`. The practical consequence is that a
desktop entry there still resolves *by name*, yet `Gio.AppInfo.get_all_for_type()`
returns empty, because the type-to-application index lives only in
`mimeinfo.cache`. Anything that must be **registered** rather than merely present
has to be mirrored into `$XDG_DATA_HOME` and have the caches rebuilt there.

That is what `decode-desktop-sync` does:

```bash
decode-desktop-sync --dry-run   # show what would change
decode-desktop-sync             # install / re-sync
decode-desktop-sync --uninstall # restore the most recent backup
```

It writes only under `$XDG_DATA_HOME` plus a few keys in `mimeapps.list`, needs
no root, backs up anything it displaces to
`$XDG_STATE_HOME/decode-desktop/backups`, and is a silent no-op when everything
is current. Desktop entries are installed only for binaries actually on `PATH`,
so a machine with just tbc-tools does not get menu entries that cannot launch.
Re-run it after upgrading, to pick up new application artwork.

The assets come from `<prefix>/share/decode-desktop`; set
`DECODE_DESKTOP_ASSET_DIR` to run against a checkout's
`src/ld-analyse/install/` instead.

A `cmake --install` to a live prefix such as `/usr/local` refreshes the caches
itself and needs no extra step. That is skipped when `DESTDIR` is set (the
packager owns the caches) or when the prefix is a nix store path; pass
`-DTBC_REFRESH_XDG_CACHES=OFF` to disable it outright.

### Entries for tools tbc-tools does not build

`decode-launcher`, `hifi-decode` and `orc-gui` ship their own desktop entries
from their own packages. The copies in `src/ld-analyse/install/` are *overrides*
that add the `%f` placeholder and the `MimeType=` list those entries lack, and
they are deliberately never installed into `share/applications` — decode-orc
installs a file of the same name into the same profile, and two packages
claiming one filename is a `nix profile add` conflict. `decode-desktop-sync`
places them in `$XDG_DATA_HOME/applications`, which wins by XDG precedence.
