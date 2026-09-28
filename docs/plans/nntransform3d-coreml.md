# Plan: nnTransform3D on CoreML (Apple Silicon)

Status: not started. Written 2026-09-28, from the macOS CI work (#47) and a read of the code
at 0491e617. It gives options with their reasoning; nothing here is decided. Line numbers
are as of that commit.

## Goal

On Apple Silicon, the `nntransform3d` chroma decoder should produce the **same output** as
the CPU path, **much faster**, by running its network on the GPU or Neural Engine through
Core ML. Today it runs on the CPU: about 12 s per frame on air0 (M1, 8 GB). That is why
the macOS ctest decodes 4 frames instead of 29: 29 frames took 5m54s, and 4 frames take
about 88 s.

## What exists today

**There is already a CoreML path. It is opt-in, and it is slower than CPU.**
- It came in with 902c9f39 (PR #18).
- `src/tbc-chroma-decoder/comb.cpp`, `appendCoreMLExecutionProvider` (about :621), appends
  ONNX Runtime's CoreML execution provider (EP) with `MLComputeUnits=ALL` and
  `ModelFormat=MLProgram`.
- It is used only when `LDDECODE_NNTRANSFORM3D_PROVIDER=coreml`. The `__APPLE__` branch
  (about :1030) explains why it is not the default:
  > ORT's CoreML EP currently rejects 3D Conv (deliberate gate in conv_op_builder.cc), so
  > for this model every Conv falls back to CPU and CoreML is a net ~7% regression.
- **Verified 2026-09-28 against ORT's source:** `onnxruntime/core/providers/coreml/builders/impl/conv_op_builder.cc`
  still rejects any Conv that isn't 1D or 2D ("Only 1D and 2D Conv are supported
  currently"). That holds in v1.23.2 (the version pinned here) and on `main`, and the latest
  release is v1.30.0.
  - The same file notes that ML Program itself supports 3D and says it will "add 3D support
    as/when needed".
  - So the limit is ORT's policy, not a Core ML limit.

**The model** is `src/tbc-chroma-decoder/resources/chroma_net_v2.onnx`, 669 KB.
- It is embedded at build time into `chroma_net_v2_onnx_data.{h,cpp}`, so nothing is looked
  up at run time.
- Exported by PyTorch 2.7.1 at opset 11. All weights are float32.
- Input `input` is `[batch, 2, 4, 16, 16]`: 2 channels (FFT magnitude and reflected
  magnitude), 4 frames, a 16×16 tile. Output `output` is `[batch, 1, 4, 16, 16]`: a mask
  applied to the complex spectrum, followed by an inverse FFT on the CPU.
- Ops:
  - **Conv ×8, all 3D:** a 1×1×1 head and tail, and 6× 3×3×3 with pad 1;
  - LeakyRelu ×7, Add ×3, Sigmoid ×1.
- **The batch dimension is dynamic;** all other dimensions are fixed.
- `src/tbc-chroma-decoder/chroma_net.onnx` is a different file with the same graph. Only
  the legacy camelCase `nnTransform3D` CUDA-kernel path loads it, from the working
  directory. It is out of scope here.

**How it runs** (`comb.cpp`):
- **One session for the whole process,** created once with `std::call_once` and shared by
  every decoder thread.
  - The threads default to `QThread::idealThreadCount()`, and all of them call `Run()`
    concurrently.
  - The environment and session are deliberately leaked: a workaround for
    microsoft/onnxruntime#24579, an OrtEnv destructor abort on macOS.
- **Session options:** `ORT_ENABLE_ALL`, plus intra-op threads from
  `LDDECODE_NNTRANSFORM3D_THREADS`. Nothing else: no model cache directory, no
  static-shape option.
- **Tiles:** 4×16×16 with an 8×8 step. Inference goes in batches of
  `PreferredInferenceBatchTiles = 32`, and a frame's last batch may be smaller.
  - The NTSC test clip is 5952 tiles, exactly 186 `Run()` calls per frame.
  - A full-frame decode is 7437 tiles: 232 full batches and one batch of 13.
- **Failure handling:** if the session can't start, or `Run()` throws, decoding silently
  falls back to 2D chroma.

**Where ONNX Runtime comes from** (`flake.nix`, `onnxruntimePackage`):
- **Darwin:** Microsoft's prebuilt `onnxruntime-osx-{arm64,x86_64}-1.23.2`.
  - It is chosen because nixpkgs' onnxruntime is built **without** the CoreML EP (checked:
    no `coremlSupport` option and no `onnxruntime_USE_COREML`).
  - 1.23.2 is also the last release with matching thin arm64 and x86_64 builds, which the
    universal DMG's `lipo` merge needs. **Upgrading ORT therefore breaks the universal DMG**
    unless that merge changes too.
- **Linux:** Microsoft's prebuilt GPU 1.18.1.
- **Windows:** downloaded 1.18.1 in the workflows.

**Tests** (`CMakeLists.txt` about :733-757): `decode-pretbc-ntsc-nntransform3d`.
- It forces `LDDECODE_NNTRANSFORM3D_PROVIDER=cpu`.
- On `APPLE` it decodes 4 frames (`--length 4 --expect-frames 4`); elsewhere 29.
- **It checks only the frame count.** It asserts neither which provider ran nor that the NN
  path didn't fall back to 2D, so a silent fallback still passes.

**Controls:**
- There is no CLI flag for the provider. Only the environment variables
  `LDDECODE_NNTRANSFORM3D_PROVIDER` (`auto|cpu|cuda|gpu|coreml`),
  `LDDECODE_NNTRANSFORM3D_THREADS` and `TBC_CUDA_PLUGIN_DIR` exist.
- None of them is documented outside the code.
- The tbc-analyse GUI has no provider selector.

## Options

### A. Rewrite the model with only 2D convolutions (recommended to try first)

Only the time axis stops Core ML from taking this model, and it holds just 4 frames. Every
3D Conv here can be rewritten exactly as a 2D Conv by folding time into channels:
- **Folding time into channels costs nothing.** `[N, C, 4, 16, 16]` reshapes straight to
  `[N, C·4, 16, 16]`: channel index `c·4 + t` matches the memory layout, so no transpose is
  needed.
- **A 3×3×3 Conv with time pad 1 becomes a 2D 3×3 Conv** from `C_in·4` to `C_out·4`
  channels. Its weights are block-banded (block Toeplitz) in time: output frame `t` sees
  input frames `t-1..t+1`, and the blocks outside the band are zero, which also gives the
  zero padding.
- **The 1×1×1 head and tail become 1×1 2D Convs** with block-diagonal weights.
- **LeakyRelu, Add and Sigmoid are element-wise,** so they are unchanged.
- **A final reshape** returns `[N, 1, 4, 16, 16]`, and the C++ code needs no change.

**Cost:** more multiply-adds, because the zero blocks are computed too.
- The 3×3×3 layers cost about 4/3× as much, since the band is 3 of 4 blocks.
- The small 1×1 head and tail cost 4× as much.
- The weights stay around a few MB.

**Why try it first:**
- Everything stays in ORT, and the existing CoreML EP path, fallbacks and tests are reused.
- It is an offline, reviewable model transform with an exact numerical check. There is no
  new native code.

**Steps:**
1. Write the rewrite as a script, for example `scripts/nn/fold_time_conv.py`, using `onnx`
   and `numpy`. Keep it and its output in the repo, next to the original model.
2. **Check that it is exact:** feed random and real inputs to the original and the
   rewritten model on ORT CPU. The max abs difference should be about 1e-6 in float32.
3. **Check the placement:** run ORT with verbose logging and the CoreML EP, and confirm
   that every node lands on CoreML. ORT logs how it partitions the graph.
4. **Make the shapes static for CoreML.** The CoreML EP handles dynamic dimensions poorly.
   - Either export with a fixed batch of 32, and pad a frame's last short batch up to 32
     in `flushPendingBatch` (padding costs little),
   - or set the CoreML EP's `RequireStaticInputShapes` and check that nothing falls back.
5. **Set a model cache directory** (`ModelCacheDirectory` in the CoreML EP options, for
   example under `~/Library/Caches/tbc-tools/coreml`), so the compiled Core ML model is
   reused between runs instead of being rebuilt at each session start.
6. **Measure on air0:**
   - CPU with the original model; CPU with the rewritten model (it may be faster or slower
     on CPU); CoreML with `MLComputeUnits` = `ALL`, `CPUAndGPU` and `CPUAndNeuralEngine`.
   - Also vary the number of decoder threads. Concurrent `Run()` calls on one CoreML
     session may serialise, so fewer threads with bigger batches could win.
7. **Check the output:**
   - The Neural Engine computes in float16, so compare decoded frames with the CPU output
     within a tolerance: PSNR or max abs difference on the `.rgb` output, not bit
     equality.
   - Agree the tolerance with Reece, because the output is client-facing.
8. **Make CoreML the default only if it wins.**
   - `auto` on `__APPLE__` would prefer CoreML and fall back to CPU.
   - Keep `LDDECODE_NNTRANSFORM3D_PROVIDER=cpu` as the escape hatch.
   - Log the provider, as today.

### B. Lift ORT's 3D gate upstream

Contribute 3D Conv support to ORT's CoreML EP. The ML Program path already supports it,
per the builder's own comment. This is the cleanest long-term fix, but it depends on
upstream timing and a new ORT release.

It also needs an ORT newer than 1.23.2 on macOS, which conflicts with the universal-DMG
constraint above. Building ORT from source on air0 (8 GB) isn't practical. Worth doing
alongside A, not instead of it.

### C. A native Core ML model, bypassing ORT on macOS

Convert the model to an `.mlpackage` and run it through the Core ML framework from an
Objective-C++ file, keeping ORT for Linux and Windows.
- **Gains:** full control over compute units, float16 and batching.
- **Costs:** a second inference path to maintain and test.
- **Conversion:** coremltools no longer converts ONNX, so it would need the original
  PyTorch model. Whether we have it is unknown; the ONNX says it came from PyTorch 2.7.1.

Only worth it if A shows ORT's CoreML EP overhead is the bottleneck.

### D. Metal / MPSGraph by hand

The most work, and not justified unless A and C both fall short.

## Risks and open questions

- **Core ML inside a Nix build on air0.**
  - ctest runs inside `nix build` (`doCheck`), as a Nix build user, with `sandbox = false`
    on air0.
  - It is untested whether Core ML, and especially the Neural Engine, is usable from a
    daemon build user with no login session.
  - If it isn't, either a CoreML test runs outside the Nix build (a separate ctest step on
    `air0-test`), or it uses `CPUAndGPU`.
- **The test should prove the NN path ran.**
  - Assert on the log line `nnTransform3D ONNX provider: <name>`, and fail on the 2D
    fallback. This is worth doing even before CoreML: today a broken ONNX path passes
    silently.
  - Add a CoreML variant of the test on macOS, and keep the CPU one.
- **Time per frame:** CPU is about 12 s per frame on air0. The 4-frame test takes 88 s, so
  roughly 40 s of it isn't per-frame work (session start, the other decode steps in
  `test-decode-pretbc`). Profile before assuming where CoreML's win will come from.
- **Universal DMG and the ORT pin:** see above. Any option that needs a newer ORT must also
  deal with the per-architecture `lipo` merge.
- **Intel Macs:** the x86_64 half of the universal DMG has no Neural Engine. There, CoreML
  would run on the GPU or CPU; measure it or keep CPU.
- **Upstream sync:** `comb.cpp` and `CMakeLists.txt` are shared with upstream (harrypm), so
  keep the C++ diff small. Option A keeps it to shapes, options and a new embedded model.

## Documentation gaps found along the way

- The provider environment variables (`LDDECODE_NNTRANSFORM3D_PROVIDER`,
  `LDDECODE_NNTRANSFORM3D_THREADS`, `TBC_CUDA_PLUGIN_DIR`) aren't documented. They belong
  in `docs/Tools/tbc-chroma-decoder.md`.
- `docs/plugins.md` (about :146-148) says the decoder reads the `cudaPlugin.*`
  configuration keys. It doesn't: `comb.cpp` only searches `cudaPluginCandidateDirectories()`.

## Definition of done (suggested)

- On air0, `nntransform3d` with the default provider runs on CoreML, and output differs
  from CPU only within the agreed tolerance.
- It is several times faster per frame than CPU. Set the target once A's measurements
  exist.
- ctest proves the NN path and the provider that ran, with the CPU and CoreML variants both
  green on air0-test.
- The macOS nnTransform3D test can go back to 29 frames without slowing CI.
