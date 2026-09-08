# AGENTS.md

This file provides guidance to AI agents when working with code in this repository.

## Project Overview

The ld-decode tools project provides professional-grade tools for digitizing, processing, and analyzing analog video sources (particularly LaserDisc captures) with exceptional quality and accuracy. The codebase consists of multiple C++ command-line tools and a shared library infrastructure.

## Development Environment

This project uses **Nix** for reproducible builds and development environments.

### Essential Commands

**Setup Development Environment:**
```bash
nix develop
```

**Build (inside Nix shell):**
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

**Build without entering shell:**
```bash
nix develop -c cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
nix develop -c ninja -C build
```

**Install without entering profile:**
```bash
nix profile install .#
```

**Run Tests:**
```bash
# Inside build directory after cmake/ninja
ctest --test-dir build --output-on-failure
```

**Clean Build:**
```bash
rm -rf build
```

## Architecture Overview

### Core Structure
- **`src/`**: All source code organized by tool
- **`src/library/`**: Shared libraries used across tools
  - **`src/library/filter/`**: Digital signal processing filters (FIR, IIR, de-emphasis)
  - **`src/library/tbc/`**: TBC format handling, metadata management, video/audio I/O
- **Individual tool directories**: Each tool has its own directory under `src/`

### Key Tools Categories
- **Core Processing**: `ld-process-vbi` (VBI decode + selectable processing options; also performs VITS metrics via `--vits` in-process)
- **EFM Decoder Suite**: `efm-decoder-f2`, `efm-decoder-d24`, `efm-decoder-audio`, `efm-decoder-data`, `efm-stacker-f2`
- **Analysis**: `ld-analyse` (GUI), `ld-discmap`, `ld-dropout-correct`
- **Export/Conversion**: `ld-chroma-decoder`, `tbc-export-metadata`, `ld-lds-converter`, `tbc-metadata-converter`

### Build System
- **CMake-based** with Ninja generator preferred
- **Out-of-source builds required** (enforced by CMakeLists.txt)
- **Multi-threading support** for performance
- **Qt6** dependency for GUI components and core functionality
- **FFTW3** for signal processing
- **SQLite** for metadata storage

### Critical Dependencies
- **ezpwd Reed-Solomon library**: Managed as git submodule at `src/efm-decoder/libs/ezpwd`
- **Qt6**: Core, Gui, Widgets, Sql modules
- **FFmpeg, FFTW, SQLite**: Via Nix or system packages

## File Format Specifications

### TBC Files
- **Binary format**: 16-bit unsigned samples, little-endian
- **Extension**: `.tbc`
- **Metadata**: Stored in separate SQLite database (`.tbc.db`)
- **Field-based**: Sequential field data with fixed width per line

### Metadata Format
- **SQLite database** format (internal, subject to change)
- **Do NOT access directly** - use `tbc-export-metadata` instead
- **Tables**: `video_parameters`, `fields`, `dropouts`

## Development Patterns

### Shared Library Usage
```cpp
// TBC metadata access
#include "tbc/lddecodemetadata.h"
LdDecodeMetaData metadata;
metadata.read("video.tbc.db");

// Video I/O
#include "tbc/sourcevideo.h"
SourceVideo source;
source.open("input.tbc", fieldWidth);

// Filtering
#include "filter/firfilter.h"
FIRFilter<double> filter(coefficients);

// Recording segments: derived only through the library. No tool re-implements
// the gap/section rule, the segment classification or the frame-range rule.
#include "tbc/segments.h"
SegmentsAnalysis analysis = analyseSegments(metadata, range, thresholds, 0.0, nullptr);
QVector<TbcMetaData::Segment> segments = deriveSegments(metadata, analysis, thresholds, "my-tool");
```

### Testing Framework
- **CTest** integration for automated testing
- **Unit tests** in `src/library/*/test*` directories
- **Integration tests** via scripts in `scripts/` directory
- **Test data** expected in `testdata/` directory (git submodule)

## Important Notes

- **SQLite metadata format is internal only** - never access `.tbc.db` files directly
- **Out-of-source builds are enforced** - use `build/` or `build-*` directories
- **Nix environment provides all dependencies** - prefer Nix over manual dependency management
- **Qt6 required** - all tools use Qt framework even for CLI tools
- **Multi-threading enabled** by default for performance-critical operations
- **Hard rule: never touch or target the ld-decode-tools upstream remote** - do not fetch from, pull from, push to, diff against, cherry-pick from, or otherwise reference `https://github.com/simoninns/ld-decode-tools.git` (or a git remote alias pointing to it, e.g. `upstream`) for any task in this project
- **Hard rule: all build/testing work must pass GitHub Actions workflows** - do not treat build verification as complete unless GitHub Actions succeeds
- **Hard rule: full-platform release-shaped builds run on demand, not on every push** - the Tests workflow (`.github/workflows/tests.yml`) must keep calling the Release workflow (`.github/workflows/release.yml`) as a reusable workflow with `create_release: false`, so the full release pipeline (Linux + Windows + macOS packaging builds) can be exercised without creating or publishing a GitHub Release. The Release workflow must support `workflow_call` with the same inputs as `workflow_dispatch`. Do not replace the reusable-workflow call with individual per-platform build workflow calls, and do not let it publish. On this fork the whole workflow is `workflow_dispatch`-only: it cost 15m26s a push, 810s of that duplicating on a hosted runner the very build `self-hosted-linux.yml` already runs on wm. Per-push guardrails come from `.github/workflows/self-hosted-guardrails.yml`, per-platform build and test coverage from the three self-hosted platform workflows, and tag pushes still reach the release pipeline directly through `release.yml`. The calling job must declare `permissions: contents: write` + `actions: write` - `release.yml` requests both, a caller job cannot grant a callee more than it holds, and this repository's default workflow permission is read-only, so omitting the block rejects the entire run at startup with "This run likely failed because of a workflow file issue" and no jobs at all.
- **Hard rule: the guardrails workflow stays unfiltered and is the only viable required check** - `.github/workflows/self-hosted-guardrails.yml` must trigger on `push` and `pull_request` with no `paths:` filter. It costs seconds on wm, and `ci/check_ci_contracts.py` validates source files as well as workflows (`src/ld-analyse/exportdialog.cpp`, tbc-video-export's field-order helpers), so gating it would create blind spots. It is also the only workflow that may be marked a required status check on `main`: the three self-hosted platform workflows and `self-hosted-deploy.yml` are all path-gated, so a pull request touching none of their paths produces no run at all, and a required check that never runs blocks the merge permanently. Enforced by `ci/check_ci_contracts.py`.
- **Hard rule: the self-hosted pipeline must not modify the harrypm build workflows** - `.github/workflows/build_linux_tools.yml`, `build_macos_tools.yml` and `build_windows_tools.yml` are shared with `harrypm/tbc-tools` and must stay byte-identical so upstream merges never conflict; none of them may contain the string `self-hosted`. The GDH self-hosted pipeline lives entirely in `self-hosted-linux.yml`, `self-hosted-macos.yml`, `self-hosted-windows.yml` and `self-hosted-deploy.yml`, which carry their own native packaging steps and reuse the shared scripts (`ci/verify_linux_bundle.sh`, `scripts/build-aaa-linux.sh`, `scripts/package-aaa-appimage.sh`, `src/tbc-video-export/pyinstaller/*`). Those four must never reference `harrypm/tbc-tools-ci-cache` or `CI_CACHE_REPO_TOKEN`, and must run with `permissions: contents: read`. Enforced by `ci/check_ci_contracts.py`.
- **Hard rule: the self-hosted deploy must never disturb the developer's working tree** - `self-hosted-deploy.yml`'s wm job installs out of the developer's real checkout at `/home/rdodge/Repos/tbc-tools`, whose profile entry is `git+file://...?ref=refs/heads/main`. Nix resolves the *ref*, not the working tree, so the job must advance `refs/heads/main` and nothing else: when `main` is the checked-out branch it fast-forwards with `git merge --ff-only` and refuses if `git status --porcelain` is non-empty (the only case where moving the ref has to go through the working tree); otherwise it runs `git fetch --no-tags origin main:main`, which without a leading `+` is fast-forward-only, and leaves whatever branch and uncommitted work the developer has completely untouched. It must then assert `refs/heads/main` is `$GITHUB_SHA` and that the resulting `nix profile list` entry reports `rev=$GITHUB_SHA`, so a stale resolve fails loudly instead of silently deploying the previous commit. The air0 job asserts the same against `github:GDH-Technologies/tbc-tools`. The win0 job must swap `%LOCALAPPDATA%\Programs\tbc-tools` atomically (rename the old tree aside, move the new one in, delete the backup, restore on failure) and refuse when a running tool holds a file lock in the install directory. Enforced by `ci/check_ci_contracts.py`.
- **Hard rule: self-hosted runners have persistent disks, not persistent workspaces** - the self-hosted workflows must not use `actions/cache` or `nix-community/cache-nix-action`; they cache to a fixed path on the box instead (`C:\ci-tools`, `$HOME/.cache/tbc-tools-selfhosted`, `$HOME/Library/Caches/tbc-tools-selfhosted`), which is correct whether or not the runner workspace survives - every bootstrapped toolchain must live outside the workspace because `actions/checkout` runs `git clean -ffdx`. They must refresh tags with `git fetch --tags --prune --prune-tags --force origin` in case a deleted or moved tag did linger and poison `git describe`, and must scope every Nix store lookup to `nix-store -qR "$(readlink -f result)"` rather than `find /nix/store`: wm and air0 carry shared multi-project stores where a store-wide search is slow and can select a different Qt6 than the one the binaries were linked against. Enforced by `ci/check_ci_contracts.py`.
- **Hard rule: do not bundle commit.txt provenance assets into releases** - release assets must contain only the platform binaries (tar.xz / zip / dmg). The `tbc-tools_${RELEASE_TAG}_commit.txt` provenance file was removed from the upload step; do not re-add it. The tag commit is already visible via the GitHub Release's tag association.
- **Hard rule: do not needlessly add helper functions** - prefer reusing existing functions and keeping logic inline unless a new helper is clearly required for readability, correctness, or reuse
- **Hard rule: GUI input contrast is mandatory** - never ship a GUI binary where text/edit/selection widgets have low-contrast text; all Qt GUI entrypoints must apply the shared `tbc::ui::enforceInputWidgetContrast(...)` guard after palette/theme setup
- **Hard rule: preserve legacy GUI theme appearance** - do not introduce blue-tinted global backgrounds; keep the established neutral grey/black theme in existing GUIs by preserving each app’s intended `QPalette::Window`/`QPalette::Base` colors
- **Hard rule: contrast guards must not restyle the full app** - shared contrast enforcement may adjust text readability roles (`Text`, `PlaceholderText`, `HighlightedText`) but must not globally override theme-defining palette roles such as `Base` and `Highlight`
- **Hard rule: all ld-analyse sub-windows open centered over the main window** - every pop-out sub-window (scopes, dialogs) must open centered over the main analyse window, not at an arbitrary window-manager position. Centering is enforced centrally by `MainWindow`'s application-wide `QEvent::Show` event filter, which calls the shared `tbc::ui::centerDialogOverParent(QWidget*)` helper (`src/library/tbc/uistyle.h`) for any `Qt::Window`/`Qt::Dialog` top-level belonging to the main window (popups/menus/tooltips are excluded). The helper accounts for the window frame margins (which otherwise leave a window shifted right/down) and clamps to the available screen geometry. New sub-windows get this for free as long as they are parented to the main window; do not add per-dialog `move()` centering that would conflict with the filter
- **Hard rule: tbc-video-export is the sole export/deinterlace engine** - ld-analyse must not hand-roll ffmpeg `bwdif`/`parity` filter graphs; proxy/web deinterlaced output must be produced via tbc-video-export web profiles (`h264_web`/`h265_web`/`av1_web`) so field-order (parity) resolution flows through `src/tbc-video-export/src/tbc_video_export/common/field_order.py`. Enforced by `ci/check_ci_contracts.py` (`LD_ANALYSE_FORBIDDEN_SNIPPETS` forbids `bwdif=mode=send_frame:parity=auto:deint=all`; `LD_ANALYSE_REQUIRED_SNIPPETS` requires the proxy web-profile routing marker + `proxyExportProfileName`).
- **Hard rule: field-order parity must default to AUTO** - `--field-order` must resolve from `firstActiveFrameLine`/`lastActiveFrameLine` + output padding (`compute_top_pad_lines`/`compute_is_tff`); never hardcode TFF/BFF in export or proxy paths. Enforced by `ci/check_ci_contracts.py` (`TBC_VIDEO_EXPORT_REQUIRED_SNIPPETS` requires `default=FieldOrder.AUTO` in `src/tbc-video-export/src/tbc_video_export/opts/opts_ffmpeg.py` and the field-order helpers in `common/field_order.py`).
- **Hard rule: CUDA is stripped from default releases; GPU acceleration is an opt-in plugin** - default release builds (Linux x86_64, Windows x86_64, macOS) build with `-DLDCHROMA_ENABLE_CUDA=OFF` (flake.nix `packages.default`), so the default Linux/tests/Windows CI jobs no longer restore the CUDA 11.8 closure or bundle CUDA runtime DLLs. The legacy custom CUDA kernel path (`nnTransform3DCUDA` + `nnTransform3D_kernel.cu`, gated by `configuration.useNNTransform3D`) is dropped from default builds (emits the existing "build does not include CUDA kernels" warning + falls back, same as macOS); production nnTransform3D keeps GPU acceleration via the ONNX Runtime CUDA EP, loaded at runtime when the CUDA plugin (a CUDA 11.8 + cuDNN 8.9 runtime DLL/SO package published to `harrypm/tbc-tools-ci-cache` Releases) is present. `flake.nix` retains `packages.cuda` (CUDA-enabled build) for the plugin-publish CI job. The `scripts/cuda-closure-cache.sh` + `scripts/windows-cuda-runtime.sh` remain required infrastructure for that job; their content is still validated by `ci/check_ci_contracts.py`. GTX-1000-series (Pascal) support is preserved (CUDA 11.8 + cuDNN 8.9; the flake's eval-time assertions still guard the pin).
- **Hard rule: Windows arm64 CI must insulate the gas-preprocessor.pl fetch from raw.githubusercontent.com** - the vcpkg `ffmpeg` port calls `vcpkg_find_acquire_program(GASPREPROCESSOR)` only for `arm`/`arm64` Windows (x64 uses NASM and is unaffected), downloading `gas-preprocessor.pl` from `raw.githubusercontent.com` which rate-limits (HTTP 429) and fails the whole Win arm64 build with `building ffmpeg:arm64-windows failed`. The `build-tbc-tools` (arm64) job in `.github/workflows/build_windows_tools.yml` must pre-fetch `gas-preprocessor.pl` (SHA512-validated, parsed from the pinned vcpkg's `GASPREPROCESSOR` program definition) from CDN mirrors (jsDelivr first, then `github.com/.../raw/`, then `raw.githubusercontent.com` with retries) into `${VCPKG_ROOT}/downloads/` before `Run CMake`, so `vcpkg_download_distfile` finds the file locally and skips the network fetch. The step is gated to `matrix.arch == 'arm64'`, best-effort (`continue-on-error`), and must appear exactly once. Enforced by `ci/check_ci_contracts.py` (`WINDOWS_GAS_PREPROCESSOR_REQUIRED_SNIPPETS` + an exact-count-==1 guard).
- **Hard rule: Windows dedicated cache repo pushes must clear checkout-injected github.com auth headers before pull/push** - `actions/checkout` injects `http.https://github.com/.extraheader` credentials for `github-actions[bot]`; in the `external-cache` repo this can override PAT-in-URL auth and cause 403 (`Permission to harrypm/tbc-tools-ci-cache.git denied to github-actions[bot]`). The cache push step in `.github/workflows/build_windows_tools.yml` must run `git config --local --unset-all http.https://github.com/.extraheader` (best-effort) and clear local credential helper before `git pull`/`git push`, so `CI_CACHE_REPO_TOKEN` is always used for cache-repo writes. Enforced by `ci/check_ci_contracts.py` (`WINDOWS_CACHE_PUSH_AUTH_REQUIRED_SNIPPETS` + exact-count guard).
- **Hard rule: Linux AAA source builds must stay xbuild-compatible and must not require apt msbuild on arm64** - Ubuntu arm64 runners do not provide an `msbuild` apt package (`E: Unable to locate package msbuild`), and Oracle Linux 8's Mono `xbuild` 14.0 cannot compile the vendored AAA project at `TargetFrameworkVersion v4.7.2` (fails with `CSC CS0518` predefined type errors). `.github/workflows/build_linux_tools.yml` must not install `msbuild` in the arm64 AAA step, and `scripts/build-aaa-linux.sh` must keep the xbuild compatibility override (`/p:TargetFrameworkVersion=v4.5` when `xbuild` is selected). Enforced by `ci/check_ci_contracts.py` (`LINUX_AAA_FORBIDDEN_SNIPPETS` + `LINUX_AAA_XBUILD_REQUIRED_SNIPPETS`).
- **Hard rule: Windows x86_64 release ships CPU-only-at-runtime; CUDA is an opt-in plugin** - the Windows x86_64 release no longer bundles the ~1.6 GB CUDA 11.8 + cuDNN 8.9 runtime DLLs; `onnxruntime_providers_cuda.dll` is still shipped (from the ORT GPU package, copied by the "Copy ONNX Runtime DLLs" step) but `LoadLibrary` fails without the CUDA runtime DLLs and ORT silently falls back to the CPU EP (intended for the default release). Users who want GPU acceleration install the CUDA plugin (Phase 4: a `tbc-tools-cuda-plugin-windows-x64.zip` published to `tbc-tools-ci-cache` Releases), which provides the CUDA runtime DLLs; the runtime loader (comb.cpp `ensureWindowsOnnxCudaProviderLoaded`, extended in Phase 2 to search the plugin dir) resolves them. arm64 uses CPU-only ONNX Runtime and is unaffected. The vendored vhs-teletext + AAA payloads are untouched. Enforced by `ci/check_ci_contracts.py` (the old `WINDOWS_CUDA_RUNTIME_REQUIRED_SNIPPETS` + `LINUX_CUDA_CACHE_REQUIRED_SNIPPETS` + `TESTS_CUDA_CACHE_REQUIRED_SNIPPETS` workflow-step requirements are removed; the script-content checks for the pinned wheel versions + `--option require-sigs false` remain).

## CUDA 11.8 closure cache

The flake vendors CUDA 11.8 from `nixpkgsLegacy` (nixos-24.11) for GTX-1000-series (Pascal) support. Nixpkgs 25.05 will remove `cudaPackages_11_8`, and `cache.nixos.org` may GC its paths. To insulate the build, the pinned closure is mirrored in the public `harrypm/tbc-tools-ci-cache` git repo as a Nix binary cache (nars > 95 MiB are chunked to fit GitHub's 100 MiB/file limit).

**Note:** default release builds no longer use this closure (`packages.default` builds with `LDCHROMA_ENABLE_CUDA=OFF`). The closure is now consumed only by `packages.cuda` / the CUDA-plugin publish CI job (Phase 4). The script + cache repo remain required infrastructure.

`scripts/cuda-closure-cache.sh` manages it:
```bash
scripts/cuda-closure-cache.sh export  --out ./cuda-cache   # realise + write the binary cache (chunk big nars)
scripts/cuda-closure-cache.sh verify  --out ./cuda-cache   # structural + sha256 check (no restore)
scripts/cuda-closure-cache.sh restore --out ./cuda-cache   # reassemble chunks + import into local Nix store
scripts/cuda-closure-cache.sh push    --out ./cuda-cache --yes   # commit + push cache to tbc-tools-ci-cache
scripts/cuda-closure-cache.sh pull    --out ./cuda-cache   # clone tbc-tools-ci-cache -> ./cuda-cache
```

The script's `NIXPKGS_REV`/`NIXPKGS_SHA` must match `flake.lock`'s `nixpkgsLegacy` rev; `flake.nix` has eval-time assertions that fail loudly if the pin drifts and drops `cudaPackages_11_8`/`cudnn_8_9`/`gcc11` or ships a non-11.8 toolkit. The local `cuda-cache/` working dir is gitignored here (the closure lives in `tbc-tools-ci-cache`, not this repo).

The cache is self-built and **unsigned**, so `restore` imports it with `nix copy --from file://... --option require-sigs false`. This is safe because the cache is local (not an untrusted network substituter), `reassemble_all` already SHA256-verifies every reassembled nar, and `require-sigs = false` only lifts the trusted-key signature check — Nix still verifies each imported nar's content hash against the narinfo's `narHash`. Without this, Nix refuses the paths (`cannot add path ... because it lacks a signature by a trusted key`) and CI silently falls back to `cache.nixos.org` (no insulation). Enforced by `ci/check_ci_contracts.py` (`CUDA_CLOSURE_CACHE_SCRIPT` must contain `--option require-sigs false`).

## Windows CUDA runtime DLL plugin package

The Windows x86_64 release ships `onnxruntime_providers_cuda.dll` (the ONNX Runtime 1.18.1 CUDA-11.x execution provider, copied from the ORT GPU package) but **does not bundle** the CUDA 11.8 runtime + cuDNN 8.9 DLLs it imports. On a clean Windows machine those are not present, so `LoadLibrary("onnxruntime_providers_cuda.dll")` fails and ORT silently falls back to the CPU EP — the intended behaviour for the default (CPU-only-at-runtime) release. GPU acceleration is an opt-in **CUDA plugin** (Phase 4: a `tbc-tools-cuda-plugin-windows-x64.zip` published to `tbc-tools-ci-cache` Releases) that provides the CUDA runtime DLLs; the Phase 2 runtime loader (comb.cpp `ensureWindowsOnnxCudaProviderLoaded`, extended to search the plugin dir) resolves them so `LoadLibrary` succeeds and the ORT CUDA EP registers.

`onnxruntime_providers_cuda.dll` directly imports `cudart64_110.dll`, `cublas64_11.dll`, `cublasLt64_11.dll`, `cufft64_10.dll` and `cudnn64_8.dll`; `cudnn64_8.dll` then lazy-loads the `cudnn_*_infer64_8.dll` sub-libraries for the op categories the model uses (the `chroma_net_v2.onnx` 3D-Conv chroma model — op set `Conv x8, LeakyRelu x7, Add x3, Sigmoid x1` — needs the cnn/ops infer sub-DLLs; `cudnn_adv_infer64_8` is dropped from the plugin as unused).

`scripts/windows-cuda-runtime.sh` fetches the DLL set from NVIDIA's official redistributable pip wheels (`nvidia-*-cu11`, `win_amd64`) — public on PyPI with no NVIDIA login (unlike `developer.nvidia.com` cuDNN) — and mirrors it (chunked under 95 MiB) into the dedicated `harrypm/tbc-tools-ci-cache` repo so CI pulls ~1.6 GB from our own cache instead of re-downloading from PyPI each run:
```bash
scripts/windows-cuda-runtime.sh fetch  --out ./win-cuda-cache            # download wheels via PyPI JSON API, extract DLLs + licenses, write manifest.txt
scripts/windows-cuda-runtime.sh verify --out ./win-cuda-cache            # check every expected DLL present + SHA256 vs manifest.txt
scripts/windows-cuda-runtime.sh push   --out ./win-cuda-cache --yes      # chunk files >95 MiB, commit into tbc-tools-ci-cache, push
scripts/windows-cuda-runtime.sh pull   --out ./win-cuda-cache            # clone tbc-tools-ci-cache, reassemble chunks, place DLLs
```

Pinned wheel versions: `nvidia-cuda-runtime-cu11==11.8.89`, `nvidia-cublas-cu11==11.11.3.6`, `nvidia-cufft-cu11==10.9.0.58`, `nvidia-cudnn-cu11==8.9.5.29` (cuDNN 8.x is required by ORT 1.18.x CUDA-11.x; 9.x is ABI-incompatible). Plugin DLLs (7, trimmed): `cudart64_110`, `cublas64_11`, `cublasLt64_11`, `cufft64_10`, `cudnn64_8`, `cudnn_cnn_infer64_8`, `cudnn_ops_infer64_8` (the `_train_` sub-DLLs are dropped — inference only; `cudnn_adv_infer64_8` is dropped — unused by the conv-only `chroma_net_v2.onnx` model). NVIDIA License.txt files are included in the plugin package. The local `win-cuda-cache/` working dir is gitignored here (the DLL set lives in `tbc-tools-ci-cache`, not this repo). Enforced by `ci/check_ci_contracts.py` (`WIN_CUDA_RUNTIME_SCRIPT` is a required file; the script must contain the pinned wheel versions; `--option require-sigs false` in the closure cache script). The workflow-step requirements (`WINDOWS_CUDA_RUNTIME_REQUIRED_SNIPPETS` etc.) were removed when CUDA was stripped from the default release.
