{
  description = "tbc-tools (Nix flake)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgsLegacy.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";
    ezpwd = {
      url = "github:pjkundert/ezpwd-reed-solomon";
      flake = false;
    };
  };
  outputs = { self, nixpkgs, nixpkgsLegacy, flake-utils, ezpwd }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgsUnstable = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        legacyPkgs = import nixpkgsLegacy {
          inherit system;
          config.allowUnfree = true;
        };
        ezpwdSrc = ezpwd;
        isLinux = pkgsUnstable.stdenv.hostPlatform.isLinux;
        isDarwin = pkgsUnstable.stdenv.hostPlatform.isDarwin;
        isLinuxX86_64 = isLinux && pkgsUnstable.stdenv.hostPlatform.isx86_64;
        enableCuda = isLinuxX86_64;
        # Every platform builds from the locked nixpkgs (Qt 6.10.1). The
        # nixpkgsLegacy pin (nixos-24.11) is kept only for what unstable has
        # removed: the CUDA 11.8 / cuDNN 8.9 / gcc11 toolchain for GTX-1000
        # (Pascal) support, and so packages.cuda, cuda-plugin-linux-deps and
        # the Linux devShell's Python/OpenCL. Don't `nix flake update` the
        # nixpkgs input casually: it sets the Qt version on Linux and macOS.
        pkgs = pkgsUnstable;
        flacPackage = pkgsUnstable.flac;
        # Vendor older CUDA package sets from legacy nixpkgs for Pascal/GTX 1000 support.
        # Keep both sets available; default to CUDA 11.8 for pre-Volta compatibility.
        vendoredCudaPackages11 = if enableCuda then legacyPkgs.cudaPackages_11_8 else null;
        vendoredCudaPackages12 = if enableCuda then legacyPkgs.cudaPackages_12_4 else null;
        cudaPackages = vendoredCudaPackages11;
        cudaHostCompiler = if enableCuda then legacyPkgs.gcc11 else null;
        cudaCudnnPackage = if enableCuda then cudaPackages.cudnn_8_9 else null;
        onnxruntimePackage =
          if isLinuxX86_64 then
            legacyPkgs.stdenvNoCC.mkDerivation rec {
              pname = "onnxruntime-gpu-prebuilt";
              version = "1.18.1";
              src = legacyPkgs.fetchurl {
                url = "https://github.com/microsoft/onnxruntime/releases/download/v${version}/onnxruntime-linux-x64-gpu-${version}.tgz";
                sha256 = "sha256-2UevDkMR/TgBKtad6kmD5zzl8XVNoNW3oRhgPdh7GX0=";
              };
              sourceRoot = "onnxruntime-linux-x64-gpu-${version}";
              dontConfigure = true;
              dontBuild = true;
              installPhase = ''
                runHook preInstall
                mkdir -p "$out"
                cp -r ./* "$out"/
                runHook postInstall
              '';
            }
          else if isDarwin then
            # Microsoft prebuilt for CoreML EP (nixpkgs lacks it). 1.23.2
            # is the last release with matching arm64+x86_64 thin builds,
            # required for the universal DMG's per-arch lipo merge.
            let
              archSuffix =
                if pkgsUnstable.stdenv.hostPlatform.isAarch64 then "arm64" else "x86_64";
              archSha256 =
                if pkgsUnstable.stdenv.hostPlatform.isAarch64
                then "sha256-tNUTqysm8IjGaJHbvBQIFmcIdz18xBY9573KDpu7eFY="
                else "sha256-0QNZ4WNHtX2ZWffoCiJaW0pm7X1+AHJ0oVyuhoNkhaY=";
            in
            pkgsUnstable.stdenvNoCC.mkDerivation rec {
              pname = "onnxruntime-osx-${archSuffix}-prebuilt";
              version = "1.23.2";
              src = pkgsUnstable.fetchurl {
                url = "https://github.com/microsoft/onnxruntime/releases/download/v${version}/onnxruntime-osx-${archSuffix}-${version}.tgz";
                sha256 = archSha256;
              };
              sourceRoot = "onnxruntime-osx-${archSuffix}-${version}";
              # stdenvNoCC omits cctools, so install_name_tool isn't on PATH.
              # autoSignDarwinBinariesHook re-signs ad-hoc in fixupPhase after
              # install_name_tool invalidates Microsoft's signature.
              nativeBuildInputs = [
                pkgsUnstable.cctools
                pkgsUnstable.darwin.autoSignDarwinBinariesHook
              ];
              dontConfigure = true;
              dontBuild = true;
              # Strip Microsoft's broken onnxruntimeTargets.cmake (declares
              # include/onnxruntime/ which the tgz doesn't ship) so
              # find_package falls back to ONNXRUNTIME_ROOT include. Also
              # rewrite the dylib's install_name from @rpath/... to its
              # absolute store path (nix convention for a prebuilt) so we
              # don't need nix-specific LC_RPATH workarounds in the built
              # installables.
              installPhase = ''
                runHook preInstall
                mkdir -p "$out"
                cp -r ./* "$out"/
                rm -rf "$out/lib/cmake"
                install_name_tool -id \
                  "$out/lib/libonnxruntime.${version}.dylib" \
                  "$out/lib/libonnxruntime.${version}.dylib"
                runHook postInstall
              '';
            }
          else
            pkgs.onnxruntime;
        cudaRuntimeDependencies = pkgs.lib.optionals enableCuda [
          cudaPackages.cuda_cudart
          cudaPackages.libcufft
          cudaPackages.libcurand
          cudaPackages.libcublas
          cudaCudnnPackage
        ];
        runtimeLibraryPath = pkgs.lib.optionalString isLinux (pkgs.lib.makeLibraryPath ([ onnxruntimePackage ] ++ cudaRuntimeDependencies));
      in
      let
        # The GDH fork version. A Nix build never sees git tags -- flakes expose
        # rev/revCount only, and .git is filtered out of src below -- so
        # .gdh-version (written and committed by scripts/gdh_version.py at bump
        # time) is the only thing here that knows it. Before the first bump the
        # file does not exist and this falls back to upstream's own declared
        # version, which is exactly what CMakeLists.txt would resolve anyway.
        #
        # Same rule as CMakeLists.txt's gdh-version block: a file naming a
        # different upstream base is stale (upstream moved, so the GDH counters
        # reset) and is ignored, as is a malformed one. Without this check, an
        # upstream sync that bumps vcpkg.json builds the new upstream's code
        # under the previous GDH version until the next bump.
        upstreamVersion = (builtins.fromJSON (builtins.readFile ./vcpkg.json)).version;
        gdhVersionFile =
          if builtins.pathExists ./.gdh-version
          then pkgs.lib.removeSuffix "\n" (builtins.readFile ./.gdh-version)
          else "";
        gdhVersionMatch = builtins.match "([0-9]+(\\.[0-9]+)*)-gdh-[0-9]+\\.[0-9]+" gdhVersionFile;
        packageVersion =
          if gdhVersionMatch != null && builtins.head gdhVersionMatch == upstreamVersion
          then gdhVersionFile
          else upstreamVersion;
        # The source the package is built from. Its store path depends only on
        # the filtered CONTENT, so the same tree gives the same path whether it
        # arrives as a git+file checkout, a PR's merge ref or a github: tarball.
        tbcSrc = pkgs.lib.cleanSourceWith {
          src = ./.;
          filter = path: type:
            let
              base = builtins.baseNameOf path;
              # The first component of the path relative to the flake root
              # ("docs/Tools/x.md" -> "docs").
              relPath = pkgs.lib.removePrefix (toString ./. + "/") (toString path);
              top = builtins.head (pkgs.lib.splitString "/" relPath);
              # Top-level entries the Nix build never reads. CMake reads src/,
              # scripts/, test-data/ and the root build files. These are CI
              # configuration, documentation, agent and developer logs, and
              # notes. Leaving them out means a docs-only or workflow-only merge
              # is not a new derivation, so wm and air0 install the build they
              # already have instead of rebuilding it.
              notBuildInput =
                builtins.elem top [
                  ".github" "docs" "development-logs" "dev-notes" "notes"
                  "AGENTS.md" "BUILD.md" "DEV_NOTES.md" "INSTALL.md" "README.md"
                  "TELETEXT_CENTERING_FIX.md" "WST_DECODER_INTEGRATION.md" "aqtinstall.log"
                ]
                || pkgs.lib.hasPrefix "prompt_readme" top;
              # The two submodule mount points. A git+file source omits a
              # gitlink entirely, but a github: tarball can carry it as an
              # empty directory -- air0's Determinate Nix does. Then the same
              # commit gets a different src and a different derivation, and the
              # deploy rebuilds what the build job already built. Neither path
              # is used by the Nix build (ezpwd comes from ezpwdSrc, and no
              # CMake file references cc_decoder), so they are always dropped.
              isSubmoduleMount = type == "directory"
                && (pkgs.lib.hasSuffix "/src/efm-decoder/libs/ezpwd" path
                    || pkgs.lib.hasSuffix "/src/tbc-process-vbi/vendor/cc_decoder" path);
            in
              !(base == ".git" || base == "build" || base == "result" || isSubmoduleMount || notBuildInput);
        };
        # The build identity comes from that content, not from the commit.
        #
        # The commit id (shortRev) and the flake ref (self.ref) used to feed
        # -DAPP_COMMIT and -DAPP_BRANCH. That made every commit a new
        # derivation, even one whose tree was byte-identical: a PR's merge ref
        # and the merge commit on main, or a version-bump commit that changes
        # only .gdh-version (which does still change the version, and so still
        # rebuilds). With a tree id, the merge deploy evaluates the derivation
        # the PR run already built, and wm and air0 install it from their local
        # store instead of recompiling.
        treeId = builtins.substring 0 12 (builtins.baseNameOf (toString tbcSrc));
        nixCommit = "src-${treeId}";
        branch = "nix";
        # mkTbcTools: single derivation factory. withCuda=false (default release,
        # CI test/release jobs) builds CPU-only - no nvcc, no CUDA buildInputs,
        # -DLDCHROMA_ENABLE_CUDA=OFF - so default `nix build .#` skips CUDA kernel
        # compilation and the pinned CUDA 11.8 closure restore. withCuda=true
        # (packages.cuda, used only by the CUDA-plugin publish job) enables the
        # legacy custom-kernel build + CUDA toolchain. Both still link the ORT GPU
        # prebuilt so the ORT CUDA EP can be loaded at runtime when the CUDA plugin
        # (runtime DLLs/SOs) is present; withCuda=false simply does not *compile*
        # the nnTransform3D_kernel.cu custom kernel (Path 2), consistent with how
        # macOS/non-CUDA builds already behave.
        # OpenCV dnn_superres models for tbc-analyse's "Save frame as PNG"
        # upscale (EDSR, ESPCN, FSRCNN, LapSRN), pinned to commits of the
        # repositories OpenCV's dnn_superres docs link to. The wrapper hands the directory to
        # tbc-analyse as TBC_SUPERRES_MODEL_DIR; a model whose files are absent
        # is simply not offered.
        superResModels = pkgs.linkFarm "tbc-superres-models" (map
          (model: { inherit (model) name; path = pkgs.fetchurl { inherit (model) url hash; }; })
          [
            { name = "EDSR_x2.pb"; url = "https://raw.githubusercontent.com/Saafke/EDSR_Tensorflow/06c7bd65b0305c2955328f8f2721ea86c341f660/models/EDSR_x2.pb"; hash = "sha256-WFYjIhuqBwJ5oNHn4ROkw/q6DzGMp/3Zpl2a/Adj2bQ="; }
            { name = "EDSR_x3.pb"; url = "https://raw.githubusercontent.com/Saafke/EDSR_Tensorflow/06c7bd65b0305c2955328f8f2721ea86c341f660/models/EDSR_x3.pb"; hash = "sha256-O6o3QP247pxS8aQdafp0y5/u8Pqb/uwk8O5YuSgGjpo="; }
            { name = "EDSR_x4.pb"; url = "https://raw.githubusercontent.com/Saafke/EDSR_Tensorflow/06c7bd65b0305c2955328f8f2721ea86c341f660/models/EDSR_x4.pb"; hash = "sha256-3TXOPK5T7O4tFgReCKkyw+ckLWQbtly5cdEj4GkENH8="; }
            { name = "ESPCN_x2.pb"; url = "https://raw.githubusercontent.com/fannymonori/TF-ESPCN/5c628eca82028161a53e1265cc3a5b571ab8625f/export/ESPCN_x2.pb"; hash = "sha256-WfdzUeHXwAV79v4Ii0qKB+QsRoyMiuu2dKa06hgjIh0="; }
            { name = "ESPCN_x3.pb"; url = "https://raw.githubusercontent.com/fannymonori/TF-ESPCN/5c628eca82028161a53e1265cc3a5b571ab8625f/export/ESPCN_x3.pb"; hash = "sha256-DmZ9t6Qx0UwyVo7Xm/IXAaZN+LHRYuSMofiACv/iwrM="; }
            { name = "ESPCN_x4.pb"; url = "https://raw.githubusercontent.com/fannymonori/TF-ESPCN/5c628eca82028161a53e1265cc3a5b571ab8625f/export/ESPCN_x4.pb"; hash = "sha256-5APwYwkinPNgCc2PsNoDK6dkP66fFc+U/lYujt+P70c="; }
            { name = "FSRCNN_x2.pb"; url = "https://raw.githubusercontent.com/Saafke/FSRCNN_Tensorflow/6a4812c4ef1c4f5947d79beafa32a05a6eb4a94d/models/FSRCNN_x2.pb"; hash = "sha256-Nmsz8AhMez8r9nJPCix3vKlPzsnXttcjidMwBzs4DVw="; }
            { name = "FSRCNN_x3.pb"; url = "https://raw.githubusercontent.com/Saafke/FSRCNN_Tensorflow/6a4812c4ef1c4f5947d79beafa32a05a6eb4a94d/models/FSRCNN_x3.pb"; hash = "sha256-79OGVagVkIxsiVTbYFLxKOdqc18d5leJTEd9DcC2RIE="; }
            { name = "FSRCNN_x4.pb"; url = "https://raw.githubusercontent.com/Saafke/FSRCNN_Tensorflow/6a4812c4ef1c4f5947d79beafa32a05a6eb4a94d/models/FSRCNN_x4.pb"; hash = "sha256-XGjRjbVhrtjq1P/t8biX6mFbqvYOv2w1+OZB+PpKIb8="; }
            { name = "LapSRN_x2.pb"; url = "https://raw.githubusercontent.com/fannymonori/TF-LapSRN/fc51c90af1b5801a357abc919160d7ff4f24b997/export/LapSRN_x2.pb"; hash = "sha256-9ZyG5oNbvKZG29gViPB4IN/jvQnjcCl2ri2QtfwPCyE="; }
            { name = "LapSRN_x4.pb"; url = "https://raw.githubusercontent.com/fannymonori/TF-LapSRN/fc51c90af1b5801a357abc919160d7ff4f24b997/export/LapSRN_x4.pb"; hash = "sha256-0+lck8r65c5ajtV86avwfy3ljajF1tZWt2Z3SWmDXuI="; }
          ]);
        mkTbcTools = { withCuda }:
          # The CUDA build stays entirely on nixpkgsLegacy (gcc13 + Qt 6.8.3),
          # beside its gcc11 CUDA host compiler, exactly as before the move to
          # unstable. That also makes `nix build .#cuda` the local check that
          # the code still compiles on Qt 6.8.3, which the upstream-hosted
          # vcpkg Windows job builds against.
          let p = if withCuda then legacyPkgs else pkgs; in
          p.stdenv.mkDerivation ({
            pname = "tbc-tools";
            version = packageVersion;
            src = tbcSrc;

            nativeBuildInputs = with p; [
              cmake
              ninja
              pkg-config
              qt6.wrapQtAppsHook
            ] ++ pkgs.lib.optionals (enableCuda && withCuda) [
              cudaPackages.cuda_nvcc
            ];

            buildInputs = with p; [
              qt6.qtbase
              qt6.qtsvg
              fftw
              flacPackage
              # flac's own flac-config.cmake does find_dependency(Ogg), and it
              # is not satisfied by flac's closure alone. Without libogg here,
              # Ogg::ogg resolves outside Nix. The sandbox hides that during
              # `nix build`; the devShell below does not (see there).
              libogg
              ffmpeg
              sqlite
              libGL
              onnxruntimePackage
              # Optional in CMake: Lanczos/bicubic resampling and dnn_superres
              # upscaling for tbc-analyse's "Save frame as PNG"
              opencv
            ] ++ pkgs.lib.optionals (enableCuda && withCuda) [
              cudaPackages.cudatoolkit
              cudaPackages.cuda_cudart
              cudaPackages.libcufft
              cudaPackages.libcurand
              cudaPackages.libcublas
              cudaCudnnPackage
            ];

            # --set-default so an operator can point tbc-analyse at other models
            qtWrapperArgs = [
              "--set-default" "TBC_SUPERRES_MODEL_DIR" "${superResModels}"
            ];

            cmakeBuildType = "Release";
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DEZPWD_DIR=${ezpwdSrc}/c++"
              # Pin the version explicitly rather than letting CMake re-derive
              # it, so the store path name and what the binaries report can
              # never disagree.
              "-DAPP_VERSION=${packageVersion}"
              "-DAPP_BRANCH=${branch}"
              "-DAPP_COMMIT=${nixCommit}"
              "-DLDCHROMA_ENABLE_CUDA=${if withCuda then "ON" else "OFF"}"
            ] ++ pkgs.lib.optionals (isLinux || isDarwin) [
              "-DONNXRUNTIME_ROOT=${onnxruntimePackage}"
            ] ++ pkgs.lib.optionals (enableCuda && withCuda) [
              "-DCUDAToolkit_ROOT=${cudaPackages.cudatoolkit}"
              "-DCMAKE_CUDA_HOST_COMPILER=${cudaHostCompiler}/bin/g++"
            ];
          } // pkgs.lib.optionalAttrs (!withCuda && isDarwin) {
            # ctest runs inside this build, so the binaries the tests exercise
            # are the ones that get installed and deployed, and CI compiles once.
            # Darwin only for now: Linux follows once it is proven on wm. The
            # attrs are merged in only here so no check attribute reaches the
            # Linux or CUDA derivations.
            doCheck = !withCuda && isDarwin;
            # Serial, and this is what makes it so: nixpkgs' cmake setup hook
            # exports CTEST_PARALLEL_LEVEL=$NIX_BUILD_CORES unless this is off,
            # and a bare `ctest` obeys it. The decode and chroma tests all
            # write testout/test, and in parallel they corrupt each other's
            # SQLite files; testconfiguration's settings checks race too.
            enableParallelChecking = false;
            nativeCheckInputs = [
              # scripts/test-* and the vendored vhs-teletext tree need these.
              (p.python3.withPackages (ps: with ps; [
                numpy scipy matplotlib click tqdm pyzmq watchdog typing-extensions
              ]))
              # scripts/test-chroma drives ffmpeg directly.
              p.ffmpeg
            ];
            # scripts/test-chroma is `#!/usr/bin/python3`, which a Nix build
            # does not have.
            postPatch = ''
              patchShebangs scripts
            '';
            # CMake configures build/bin/tbc-video-export with an /usr/bin/env
            # shebang, and the Qt tests write settings under the home
            # directory. On macOS QStandardPaths asks Foundation, which takes
            # the build user's account home (/var/empty, read-only for
            # _nixbld) rather than $HOME -- testconfiguration's writes then
            # vanish -- unless CFFIXED_USER_HOME overrides it. The teletext
            # test runs Python straight from the source tree, which
            # installPhase copies after this phase, so no bytecode may be
            # written or __pycache__ ships in the package.
            preCheck = ''
              export HOME=$TMPDIR
              export CFFIXED_USER_HOME=$TMPDIR
              export PYTHONDONTWRITEBYTECODE=1
              patchShebangs bin
            '';
            # Serial (see enableParallelChecking above).
            checkPhase = ''
              runHook preCheck
              ctest --output-on-failure
              runHook postCheck
            '';
          });
      in
      assert pkgs.lib.assertMsg
        (!enableCuda || pkgs.lib.versionAtLeast cudaPackages.cudatoolkit.version "11.8")
        "CUDA toolkit must be >= 11.8";
      assert pkgs.lib.assertMsg
        (!enableCuda || pkgs.lib.versionAtLeast vendoredCudaPackages12.cudatoolkit.version "12.4")
        "Vendored CUDA 12 package set must provide toolkit >= 12.4";
      # Pre-empt the Nixpkgs 25.05 removal of cudaPackages_11_8: if the
      # nixpkgsLegacy pin is ever bumped past a rev that drops it (or drops
      # cudnn_8_9 / gcc11), fail loudly at eval time instead of silently
      # breaking GTX-1000-series (Pascal) CUDA builds.
      assert pkgs.lib.assertMsg
        (!enableCuda || legacyPkgs ? cudaPackages_11_8)
        "nixpkgsLegacy no longer provides cudaPackages_11_8. Re-pin nixpkgsLegacy to a Nixpkgs rev that still ships it (e.g. nixos-24.11) to preserve GTX-1000-series CUDA support.";
      assert pkgs.lib.assertMsg
        (!enableCuda || (cudaPackages != null && cudaPackages ? cudatoolkit && cudaPackages ? cudnn_8_9 && legacyPkgs ? gcc11))
        "Vendored CUDA 11.8 package set is incomplete (missing cudatoolkit, cudnn_8_9, or gcc11). The nixpkgsLegacy pin may have drifted.";
      assert pkgs.lib.assertMsg
        (!enableCuda || pkgs.lib.hasPrefix "11.8" cudaPackages.cudatoolkit.version)
        "CUDA toolkit must be 11.8.x for GTX-1000-series (Pascal) support — got ${cudaPackages.cudatoolkit.version}";
      {
        packages.ffmpeg = pkgs.ffmpeg;
        packages.default = mkTbcTools { withCuda = false; };
        # CUDA-enabled build (legacy custom kernel + CUDA toolchain). Only
        # available where the pinned CUDA 11.8 closure exists (Linux x86_64);
        # used by the CUDA-plugin publish CI job, not by default releases.
        packages.cuda = if enableCuda then mkTbcTools { withCuda = true; } else null;
        # Staged CUDA 11.8 runtime + cuDNN 8.9 .so files for the Linux x86_64
        # CUDA plugin package. cuDNN 8.9 has no Linux PyPI wheel (only win_amd64),
        # so the Linux plugin package sources its .so files from the Nix store
        # (the pinned cudaPackages_11_8 / cudnn_8_9). The publish CI job does
        # `nix build .#cuda-plugin-linux-deps` and passes the output to
        # scripts/cuda-plugin-package.sh build-linux --deps-dir. Only the ORT
        # CUDA EP provider .so is fetched separately (from the ORT GPU prebuilt).
        packages.cuda-plugin-linux-deps =
          if enableCuda
          then legacyPkgs.runCommand "cuda-plugin-linux-deps" { } ''
            mkdir -p $out
            cp ${cudaPackages.cuda_cudart.lib}/lib/libcudart.so.11.0 $out/
            cp ${cudaPackages.libcublas.lib}/lib/libcublas.so.11 $out/
            cp ${cudaPackages.libcublas.lib}/lib/libcublasLt.so.11 $out/
            cp ${cudaPackages.libcufft.lib}/lib/libcufft.so.10 $out/
            cp ${cudaPackages.libcurand.lib}/lib/libcurand.so.10 $out/
            cp ${cudaCudnnPackage.lib}/lib/libcudnn.so.8 $out/
            cp ${cudaCudnnPackage.lib}/lib/libcudnn_cnn_infer.so.8 $out/
            cp ${cudaCudnnPackage.lib}/lib/libcudnn_ops_infer.so.8 $out/
          ''
          else null;

        devShells.default =
          # The Linux devShell's Python and OpenCL stay on nixpkgsLegacy:
          # pycuda/pyopencl there build against the pinned CUDA 11.8, and one
          # package set keeps a single python3 interpreter and site-packages.
          let pyPkgs = if isLinux then legacyPkgs else pkgs; in
          pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            # scripts/cuda-plugin-package.sh patches the staged cuDNN libs.
            # Not `patchelf`: nixpkgs' 0.15.x corrupts DT_RELR binaries.
            pkgsUnstable.patchelfUnstable
            qt6.qtbase
            qt6.qtsvg
            fftw
            flacPackage
            # Required, not cosmetic. src/ld-lds-converter/CMakeLists.txt does
            # find_package(FLAC CONFIG), and nixpkgs' flac-config.cmake then
            # does find_dependency(Ogg). With no libogg in the shell that
            # resolves to the host's /usr/lib64/libogg.so on a distro that has
            # one, which puts `-Wl,-rpath,/usr/lib64` on the link line -- and
            # ld then resolves the host libQt6DBus.so.6 / libglib-2.0.so.0
            # against nix Qt, failing with a wall of Qt_6.11_PRIVATE_API and
            # free_sized@GLIBC_2.43 undefined references. Reproduced on Fedora
            # 44 (system Qt 6.11); invisible on a distro with no system Qt6.
            libogg
            ffmpeg
            sqlite
            libGL
            pyPkgs.python3
            pyPkgs.python3Packages.numpy
            pyPkgs.python3Packages.scipy
            pyPkgs.python3Packages.matplotlib
            pyPkgs.python3Packages.click
            pyPkgs.python3Packages.tqdm
            pyPkgs.python3Packages.pyzmq
            pyPkgs.python3Packages.watchdog
            pyPkgs.python3Packages.pyserial
            onnxruntimePackage
            opencv
          ] ++ pkgs.lib.optionals enableCuda [
            cudaPackages.cudatoolkit
            cudaPackages.cuda_nvcc
            cudaPackages.cuda_cudart
            cudaPackages.libcufft
            cudaPackages.libcurand
            cudaPackages.libcublas
            cudaCudnnPackage
            legacyPkgs.python3Packages.pycuda
            legacyPkgs.python3Packages.pyopencl
            legacyPkgs.ocl-icd
            legacyPkgs.pocl
            legacyPkgs.clinfo
          ];
          EZPWD_DIR = "${ezpwdSrc}/c++";
          ONNXRUNTIME_ROOT = "${onnxruntimePackage}";
          CUDAToolkit_ROOT = pkgs.lib.optionalString enableCuda "${cudaPackages.cudatoolkit}";
          CUDA_PATH = pkgs.lib.optionalString enableCuda "${cudaPackages.cudatoolkit}";
          shellHook = pkgs.lib.optionalString enableCuda ''
            export CUDATOOLKIT_ROOT="$CUDAToolkit_ROOT"
            export CUDAHOSTCXX="${cudaHostCompiler}/bin/g++"
            export CMAKE_CUDA_HOST_COMPILER="${cudaHostCompiler}/bin/g++"
            for nvidiaCudaLib in \
              /usr/lib/x86_64-linux-gnu/libcuda.so.1 \
              /usr/lib64/libcuda.so.1 \
              /usr/lib/libcuda.so.1; do
              if [ -f "$nvidiaCudaLib" ]; then
                if [ -n "$LD_PRELOAD" ]; then
                  export LD_PRELOAD="$nvidiaCudaLib:$LD_PRELOAD"
                else
                  export LD_PRELOAD="$nvidiaCudaLib"
                fi
                break
              fi
            done
            if [ -n "$CUDAHOSTCXX" ]; then
              if [ -n "$NVCC_PREPEND_FLAGS" ]; then
                export NVCC_PREPEND_FLAGS="-ccbin $CUDAHOSTCXX $NVCC_PREPEND_FLAGS"
              else
                export NVCC_PREPEND_FLAGS="-ccbin $CUDAHOSTCXX"
              fi
            fi
            export OZ_OPENCL_VENDOR_DIR="$(mktemp -d -t tbc-opencl-vendors-XXXXXX)"
            for icdFile in "${legacyPkgs.pocl}/etc/OpenCL/vendors/"*.icd; do
              [ -f "$icdFile" ] && cp -f "$icdFile" "$OZ_OPENCL_VENDOR_DIR/"
            done
            for nvidiaOpenclLib in \
              /usr/lib/x86_64-linux-gnu/libnvidia-opencl.so.1 \
              /usr/lib64/libnvidia-opencl.so.1 \
              /usr/lib/libnvidia-opencl.so.1; do
              if [ -f "$nvidiaOpenclLib" ]; then
                printf "%s\n" "$nvidiaOpenclLib" > "$OZ_OPENCL_VENDOR_DIR/nvidia-abs.icd"
                break
              fi
            done
            export OCL_ICD_VENDORS="$OZ_OPENCL_VENDOR_DIR"
            export OPENCL_VENDOR_PATH="$OCL_ICD_VENDORS"
            export LD_LIBRARY_PATH="${legacyPkgs.ocl-icd}/lib:${runtimeLibraryPath}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
          '';
        };
      }
    );
}