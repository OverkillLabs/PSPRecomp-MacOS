# VCSNative: GTA Vice City Stories for macOS and Windows (PSPRecomp fork)

This is a fork of [jessicanataliagta/PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp),
the original PSPRecomp created by **Jessica Natalia**
([@jessicanataliagta](https://github.com/jessicanataliagta)). It adds a
**native macOS ARM (Apple Silicon) port** and a **Windows Vulkan build** of the VCS profile (GTA: Vice City
Stories), alongside the original Windows/DirectX12 build. None of this would exist without her original recompiler,
VCS profile and DX12 host to build on top of — all credit for the framework and the game profile itself belongs to
her. All builds share the same recompiler framework and profile; only the GE (graphics) backend and the host
platform layer differ.

- **macOS (Apple Silicon):** a native Vulkan/MoltenVK GE backend
  (`profiles/vcs/host/ge_gpu_backend_vulkan.cpp`), CoreGraphics-based display and resolution handling, and a native
  SwiftUI launcher. The finished `VCSNative.app` is a single self-contained app: FFmpeg, SDL and MoltenVK are inside
  it, so nothing has to be installed.
- **Windows (experimental):** the same Vulkan backend running natively on Windows, with a single-exe launcher
  (`VCSLauncher.exe`) and every DLL the game needs in the download. The original DirectX 12 backend is still in the
  source.
- Both platforms support keyboard and mouse as well as gamepads, and both launchers edit `VCSNative.ini` and
  `ProperShaders.ini` for you, so nothing has to be edited by hand.

Ready-to-play builds are on the [Releases page](../../releases); see "Download and play" below.

PSPRecomp itself is a static recompilation framework for PSP software. It
reads an Allegrex/MIPS executable, analyzes guest code, emits C++
translation units, and runs them through a native host runtime instead of
shipping a PSP interpreter or JIT.

The repository is split between a reusable framework and game-specific profiles. The first working profile is GTA: Vice City Stories (`profiles/vcs`).

## Download and play

Nothing has to be installed and no terminal is needed. Get the latest build from the
[Releases page](../../releases), unzip it, and open the launcher.

You need your own legally obtained copy of the game, extracted to a folder called `PSP_DATA` (it holds
`PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF` and `PSP_GAME/USRDIR`). No game files are included.

**macOS (Apple Silicon, macOS 13 or later)**
1. Unzip `VCSNative-macOS.zip` and double-click **VCSNative**. The first time, macOS asks whether you want to open an
   app downloaded from the Internet: click **Open**. (On recent macOS versions, if it only offers "Done", open
   System Settings > Privacy & Security and click **Open Anyway**.)
2. On the Window page, next to *Game folder*, click **Choose...** and pick your `PSP_DATA` folder. It can be anywhere,
   including an external drive; nothing is copied. **Play** stays greyed out until the game files are found.
3. Press **Play**. Settings, saves and textures are kept in `~/Library/Application Support/VCSNative`.

**Windows 10/11 (64-bit)**
1. Unzip the whole `VCSNative` folder somewhere you can write to (not `Program Files`).
2. Put your `PSP_DATA` folder right next to `VCSNative.exe` and `VCSLauncher.exe`.
3. Double-click **VCSLauncher.exe** and press **Play**. If Windows shows "Windows protected your PC", click
   *More info* and then *Run anyway*. You need a graphics driver with Vulkan 1.1 support, which the normal NVIDIA,
   AMD and Intel drivers include; everything else is in the folder.

The optional texture pack is a separate download: install it from the launcher's Quality page.

## Repository layout

```text
include/psprecomp/   Public runtime and Allegrex interfaces
src/                 ELF/PRX loading, decoder, memory, runtime and support code
tools/               Generic analyzer, recompiler and reverse-engineering helpers
tests/               Framework regression tests
configs/             Generic examples and PSP NID data
profiles/            Game-specific hosts, generated code, configuration and tests
  vcs/               GTA: Vice City Stories profile
```

Game-specific addresses, HLE behavior, native fast paths, renderer integration and generated AOT code belong under a profile. The framework should remain usable without any profile selected.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- Visual Studio 2022 for the Windows/DX12 VCS build
- macOS 13+ on Apple Silicon, Xcode command line tools (for `swiftc`, used
  to build the launcher), and `brew install cmake vulkan-headers vulkan-loader
  molten-vk shaderc sdl2` for the Vulkan/MoltenVK VCS build. These are needed to
  *build*; the app you build contains everything it needs to run.

## Build the framework only

```bash
cmake -S . -B out/framework -DPSPRECOMP_PROFILE=""
cmake --build out/framework --config Release
ctest --test-dir out/framework -C Release --output-on-failure
```

This builds `psprecomp_core`, `psp_analyze`, `psp_recomp`, `dump_function` and the framework tests.

## Build a profile

Profiles are selected with `PSPRECOMP_PROFILE`:

```bash
cmake -S . -B out/vcs -DPSPRECOMP_PROFILE=vcs
cmake --build out/vcs --config Release
```

Windows users working on the VCS profile can use the maintained scripts in `profiles/vcs/scripts`.

On macOS, one command builds everything and produces a ready-to-play app:

```bash
profiles/vcs/scripts/build_macos.sh
```

It builds an LGPL FFmpeg (downloaded from ffmpeg.org, so nothing GPL ends up inside the app), the game and the
launcher, then assembles **`out/vcs/dist/VCSNative.app`**: one self-contained, signed app with FFmpeg, SDL, the Vulkan
loader and MoltenVK inside it, plus `out/vcs/dist/VCSNative-macOS.zip` to share. Double-click it; see "Download and
play" above. A plain `cmake --build` produces the same `dist/VCSNative.app`. The individual `VCSNative.app` and
`VCSLauncher.app` in `bin/Release/` are development outputs.

On Windows, `profiles\vcs\BUILD_VCS.bat` produces a folder that also runs on any PC: the FFmpeg and Visual C++ runtime
DLLs are copied next to the exe.

## Create another profile

See [`docs/PROFILE_GUIDE.md`](docs/PROFILE_GUIDE.md). A new title normally provides its own generated corpus, HLE/profile host, configuration, tests and optional native fast paths without modifying the framework for game-specific addresses.

## Game files

No commercial game executable or asset is included. PSPRecomp does not ship an EBOOT decryption implementation. Profiles expect files obtained from the user's own copy in the format documented by that profile.

## Source provenance

See [`docs/SOURCE_PROVENANCE.md`](docs/SOURCE_PROVENANCE.md) for the project rules around independently written code, profile boundaries, decryption and third-party source.

## Third-party code

The framework is MIT licensed. Individual profiles may include separately licensed dependencies or assets; their notices stay beside those files. The VCS profile lists its bundled dependencies in `profiles/vcs/THIRD_PARTY.md`.

## License

PSPRecomp framework code is distributed under the MIT License. See [`LICENSE`](LICENSE).
