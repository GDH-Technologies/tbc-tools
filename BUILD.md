# Build

This project uses a Nix dev shell to provide a consistent build environment (recommended). Manual builds against native distro packages are documented at the bottom of this file.

## Enter the dev shell

```bash
nix develop
```

This exposes all build dependencies (CMake, Ninja, Qt6, FFmpeg, FFTW, SQLite, OpenGL, etc.).

## Configure and build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Artifacts will be under `build/`.

## Build without entering the shell (one-off)

```bash
nix develop -c cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
nix develop -c ninja -C build
```

## Optional: clean build

```bash
rm -rf build
```

## Notes

- The flake sets `-DEZPWD_DIR`, `-DAPP_BRANCH`, and `-DAPP_COMMIT` automatically for package builds.
- The dev shell exports `EZPWD_DIR`, so manual builds via `nix develop` pick it up automatically.

# Manual build (native distro packages)

Before building, ensure submodules are present (required for `ezpwd`):

```bash
git submodule update --init --recursive
```

Install dependencies for your distro family:

Package names can vary slightly by distro release; if one package name differs, install the equivalent Qt6/FFTW/SQLite/FFmpeg development package for your release.

## Debian/Ubuntu (including Linux Mint)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev qt6-tools-dev qt6-tools-dev-tools libqt6svg6-dev libfftw3-dev libsqlite3-dev ffmpeg libgl1-mesa-dev python3 python3-numpy
```

## Fedora

```bash
sudo dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config qt6-qtbase-devel qt6-qtsvg-devel fftw-devel sqlite-devel ffmpeg ffmpeg-devel mesa-libGL-devel python3 python3-numpy
```

## Arch Linux / EndeavourOS / Manjaro

```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-svg fftw sqlite ffmpeg mesa python python-numpy
```

## openSUSE Tumbleweed/Leap

```bash
sudo zypper install -y gcc-c++ cmake ninja pkgconf-pkg-config qt6-base-devel libqt6-qtsvg-devel fftw3-devel sqlite3-devel ffmpeg Mesa-libGL-devel python3 python3-numpy
```

## Build and install

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

If you do not want to install system-wide, run tools directly from `build/bin/`.
