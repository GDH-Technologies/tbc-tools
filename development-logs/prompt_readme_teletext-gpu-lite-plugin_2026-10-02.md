# Prompt readme — teletext GPU fix + lite CUDA plugin (2026-10-02)

## User prompt(s)
- "I would like to see some decoded results"
- "Fix the cuda system and add it to plugins for a lite cuda integration as this was working after the inital implimentation"
- "Execute this plan." (plan approved: Teletext GPU acceleration fix + lite CUDA (OpenCL) plugin)

## Problem (verified with hard data)
- Installed tbc-tools resolves `/usr/bin/python3` (3.10.12): no pycuda/pyopencl -> teletext deconvolution falls back to CPU
  (`No module named 'pycuda'`, `OpenCL init failed.`, `CUDA init failed.`)
- nix develop shell (Python 3.12.8) has pycuda/pyopencl/pocl -> GPU worked there (the "initial implementation" behaviour)
- User GPU: NVIDIA GeForce GTX 1070 Ti, driver 580.178.04, libcuda.so.1 + libOpenCL.so.1 present

## Key design constraint
- `pycuda` publishes NO Linux wheels on PyPI (source-only, needs nvcc) -> portable pycuda plugin not feasible
- `pyopencl` publishes manylinux (cp310-cp313) + win_amd64 wheels; its manylinux wheel BUNDLES the OpenCL ICD loader
  (`pyopencl/.libs/libOpenCL-5519dec5.so.1.0.0`, verified via ldd) -> only a driver ICD (/etc/OpenCL/vendors) is needed
- vhs-teletext deconvolver auto-detects backends and runs GPU-accelerated via OpenCL when pyopencl + ICD are present
- => "lite" plugin = pyopencl wheel package; the CUDA/OpenCL driver stack comes from the user's GPU driver

## Implementation
1. `src/tbc-process-vbi/teletextintegration.cpp`
   - `resolveCudaLitePluginDirectory()`: finds `<GenericDataLocation>/tbc-tools/plugins/tbc-tools.cuda-lite-<platform>-<arch>`
     (requires `site-packages/` subdir)
   - Plugin `site-packages` prepended to the teletext subprocess `PYTHONPATH`
   - `resolvePythonExecutable(cudaLitePluginDirectory)` prefers a plugin-bundled `python/python.exe` (Windows embeddable) /
     `python/bin/python3`, after the `TELETEXT_PYTHON` override
   - With pyopencl usable + no pycuda -> OpenCL-first deconvolution ordering (skips the noisy guaranteed-failed CUDA attempt);
     `TELETEXT_PREFER_OPENCL` / `TELETEXT_FORCE_CPU` / `TELETEXT_PYTHON` overrides still win
2. `src/tbc-analyse/plugincatalog.{h,cpp}` + `pluginmanagerdialog.cpp`
   - New optional catalog schema field `platforms` (`<platform>-<arch>` gate); dialog hides entries not shipping for the
     current platform (empty list = all platforms)
3. `scripts/cuda-plugin-package.sh`
   - New modes: `build-cuda-lite-linux` (pyopencl wheels cp310-313 + pure deps; 2.0 MB; numpy NOT bundled — the teletext
     deps probe requires it anyway) and `build-cuda-lite-windows` (Python 3.13.9 embeddable + `._pth` removed so
     PYTHONPATH works + full pyopencl closure incl. numpy; 24 MB); `build-cuda-lite-all` builds both
4. `plugins/catalog.json` — two new generic entries (id `tbc-tools.cuda-lite-linux-x86_64` / `-windows-x86_64`,
   version 1.0.0, package_url -> tbc-tools-ci-cache release `cuda-lite-plugin-v1.0.0`, full per-file files[] manifests:
   109 Linux files / 1087 Windows files)
5. `.github/workflows/publish_cuda_plugin.yml` — new `build-and-publish-cuda-lite` job (wheels-only, no Nix closure);
   tag prefixes `cuda-plugin-v*` / `cuda-lite-plugin-v*` with job-level `if:` guards so they never cross-trigger
6. Contracts: `ci/check_ci_contracts.py` — lite snippets added to `CUDA_PLUGIN_PUBLISH_REQUIRED_SNIPPETS` +
   new `CUDA_LITE_PACKAGE_SCRIPT_REQUIRED_SNIPPETS`; tests in `ci/tests/test_check_ci_contracts.py`
7. Docs: `docs/plugins.md` (platforms schema field + "Teletext GPU Runtime (lite) plugin" section), AGENTS.md
   ("Teletext GPU runtime (lite) plugin" section)

## Verification results (hard data)
- Package build: Linux tar.gz 2.0 MB / manifest 109 files; Windows zip 24 MB / manifest 1087 files — both valid JSON
- Local plugin install: `~/.local/share/tbc-tools/plugins/tbc-tools.cuda-lite-linux-x86_64/`
- GPU decode of `/media/harry/20TB HDD1/Testing_Data/AG4700 -Testing/Teletext.tbc` (PAL, 540 fields) with build binary:
  - `Teletext export: GPU runtime plugin (tbc-tools.cuda-lite-linux-x86_64) found: /home/harry/.local/share/tbc-tools/plugins/tbc-tools.cuda-lite-linux-x86_64`
  - Probe: `pycuda=no`, `pyopencl=yes`, `pyopencl_runtime=yes`, `pyopencl_selected_ctx=0:0`
  - `OpenCL-first backend ordering enabled by GPU runtime plugin`
  - `Teletext export complete - generated 247 HTML pages` — identical page count to the CPU baseline run
- Decoded content sampled: Rai "TELEVIDEO" pages (100 index, 103/120 news, Italian politics headlines)
- Gates: `ci/check_ci_contracts.py` passed; contract tests 48 passed; ctest 30/30

## Follow-ups
- Publish the actual plugin packages + catalogs: push tag `cuda-lite-plugin-v1.0.0` (or dispatch the publish workflow)
  — held until the user gives the go-ahead
- Real-world GUI validation: install the plugin via Plugin Manager on a clean machine + confirm GPU probe in the GUI log
- Vendored vhs-teletext update note (pinned f470629) still pending as a separate follow-up
