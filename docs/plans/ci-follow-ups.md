# Plan: CI follow-ups left over from the test-runner work

Status: not started. Written 2026-09-28, after the test-runner series (#47, #48, #49, #50,
#51, #52). Each section stands on its own. They are suggestions with their reasoning, not
decisions.

## Background: where CI stands

- **Every platform's compile and ctest run on that box's dedicated test runner:**
  - `air0-test` for macOS;
  - `wm-test` for Linux;
  - `win0-test` for Windows.
- **The main runners (`air0`, `wm`) only package.**
  - They reuse the test job's Nix build, which the GC roots keep alive.
  - `win0` only installs deploys.
- **ctest runs inside the Nix build** (`flake.nix`: `doCheck = !withCuda`), serially.
- **Typical times:**
  - macOS: about 7-9 min per run, of which about 2.5 min is on `air0`;
  - Linux: about 1.5 min on a store hit, about 5 min with a compile;
  - Windows: about 8 min on a cold build directory.
- **air0 constraints:** an M1 MacBook Air with 8 GB of RAM, whose internal disk sat at
  85-89 % in September 2026. Low free space makes Nix's GC evict the next build's inputs,
  and a re-fetch can add minutes. That is why the package job clones partially (#52).

## 1. Per-test output directories, so ctest can run in parallel

**Why ctest is serial today.** The decode and chroma tests all write to one shared
`testout/` directory. Run in parallel, they overwrite each other's files, and 7 of them
failed.
- The Nix build therefore sets `enableParallelChecking = false`. nixpkgs' cmake hook would
  otherwise export `CTEST_PARALLEL_LEVEL`.
- On Linux the serial ctest takes about 2m26s.

**Idea:**
- Give each test its own output directory, for example `testout/<test-name>/`, through the
  arguments in `CMakeLists.txt` and `scripts/test-decode-pretbc`.
- Then set `enableParallelChecking = true` and give the SQLite-writing tests a ctest
  `RESOURCE_LOCK` if they still collide.

**Caveats:**
- **air0 has 8 GB of RAM.** Parallel decode tests, including nnTransform3D, may swap, so
  measure there before enabling this on Darwin. It can be turned on for Linux alone.
- **This is a derivation change,** so it rebuilds every platform once.
- **Upstream sync.** `CMakeLists.txt` and `scripts/test-decode-pretbc` are shared with
  upstream, so keep the diff small.

## 2. Partial clones for other repositories' jobs on air0

On 2026-09-28, the `air-0` runner's work directory still held:
- `vhs-decode`: 1.4 GB;
- `capture-node`: 0.7 GB;
- `digitization-toolkit`: 0.26 GB.

Those are those repositories' own workflows. If any of them does a full-history
`actions/checkout`, the tbc-tools fix applies there too:
- `fetch-depth: 0` with `filter: blob:none` when history or tags are needed;
- `fetch-depth: 1` when they are not;
- no submodules unless the job reads them.

Existing workspaces keep their old objects: `actions/checkout` reuses a `.git`. So each
workspace has to be deleted once, while `air-0` is idle, before the first partial-clone run.

## 3. If air0 CI should ever use the portable SSD

This was decided against on 2026-09-28 in favour of freeing internal space (#52).

**Why:**
- The SSD (`/Volumes/1TBPortable`) is **exFAT** and is not always attached.
- The tbc-tools CI caches are about 60 MB, so moving them saves nothing.
- Nix GC roots must stay on the internal disk. A root on an unmounted volume is dangling,
  so Nix drops it and collects the build.
- The Nix store cannot move.

**If it is revisited, copy the existing Xcode setup rather than a raw exFAT path:**
- an APFS sparsebundle on the SSD (`Xcode/XcodeData.sparsebundle`, mounted at
  `/Volumes/XcodeData`);
- re-attached by a user LaunchAgent (`com.gdhvc.mount-xcodedata`), which runs
  `hdiutil attach -nobrowse` every 60 s while the SSD is present.

**Runner work directories** are fixed per runner (`.runner`'s `workFolder`), so they can't
switch disks per job:
- the switch has to happen at runner start, or from a watcher that repoints `_work` only
  while the runner is idle;
- the image must be detached before the SSD is ejected;
- pulling the SSD mid-job fails that job.

## 4. nnTransform3D on CoreML

This has its own plan: [nntransform3d-coreml.md](nntransform3d-coreml.md).
