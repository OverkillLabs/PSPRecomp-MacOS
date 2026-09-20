# Windows + Vulkan: parity with the macOS build

Written on a Mac. **Nothing in the Windows-only files has been compiled or run by its author.** This
document is the handoff for whoever builds and tests it on Windows: what exists, what is unverified,
and exactly how to verify it.

## The rules (from the project owner)

1. **Parity in both directions.** macOS and Windows get the same features, quality and performance.
   A feature is implemented in shared code (`profiles/vcs/host/`) or in *both* platform layers.
   Anything found on one side and missing on the other is a bug to fix, not a difference to keep.
2. **Windows and macOS code are separate.** OS-specific code lives only in
   `profiles/vcs/platform/<os>/`. Shared code must not contain `#if defined(_WIN32)` or
   `#if defined(__APPLE__)` for anything that has a platform file. Changing one platform's file
   must never be able to change the other.
3. **The build picks the platform by itself.** No manual flags: macOS builds the macOS layer,
   Windows builds the Windows layer.
4. **Do not break the DirectX 12 build.** It is the older, proven Windows backend. Anything new that
   was only validated with Vulkan is gated on `ge_gpu_backend_is_vulkan()` at run time (never on a
   compile flag: a flag change rebuilds all 234 generated AOT units, which is very slow with MSVC).
5. Never commit, push or release without the owner's explicit approval. Keep CRLF in
   `vcs_profile.cpp` and `ge_renderer.cpp` (edit with `newline=''` in Python).

## Why Vulkan on Windows

The DirectX 12 backend has **none** of the graphics stack or the texture pipeline (no bloom, FXAA,
SMAA, colour grading, sky palette, sun relief, VHS, texture replacement, HUD texture handling, scale
handling). Those were built in `ge_gpu_backend_vulkan.cpp` on macOS. Full parity on Windows therefore
means running the Vulkan backend. Vulkan on Windows already ran before this work (through the CPU
readback path); what was missing is parity and performance.

## Layout

```
profiles/vcs/
  host/                      shared, platform-neutral (VCS HLE, GE renderer, backends, config, ...)
    ge_gpu_backend_vulkan.cpp  the Vulkan backend: NO OS ifdefs; asks vulkan_platform.hpp
    ge_gpu_backend_dx12.cpp    the older DirectX 12 backend (Windows only)
    vulkan_platform.hpp        the interface between the Vulkan backend and an OS
  platform/
    macos/    vulkan_platform_metal.cpp (CAMetalLayer surface), display_window_sdl.cpp (SDL2 window,
              input, present), metal_drawable_size.{h,mm}
    windows/  vulkan_platform_win32.cpp (HWND surface), display_window_win32.cpp (Win32 window,
              raw input, XInput, DX12 presenter)
    generic/  stubs for other platforms (readback present, no window)
  scripts/    Windows build and helper .bat files (INSTALL_TEXTURE_PACK.bat, UNINSTALL_TEXTURE_PACK.bat)
```

`CMakeLists.txt` compiles exactly one file from each `platform/` group. Backend choice (Windows):
Vulkan if the Vulkan SDK (`glslc`) is found, otherwise DirectX 12 with a loud warning. Override with
`-DPSPRECOMP_VCS_GE_BACKEND=VULKAN` or `DX12`.

## What changed in this round (all uncommitted)

- Vulkan backend made platform-neutral. The real swapchain present path (no CPU readback), which was
  `#if __APPLE__`, now compiles everywhere and reaches the OS only through `vulkan_platform.hpp`.
- New Win32 surface layer (`vulkan_platform_win32.cpp`, syntax-checked on macOS against the real Vulkan
  headers with a stand-in `windows.h`; never compiled with MSVC).
- **Swapchain recreation** on resize / minimise / `VK_ERROR_OUT_OF_DATE_KHR` (needed on Windows; macOS
  never resizes). The extent is polled every frame where `surface_extent_can_change()` is true.
- **GPU selection**: prefers discrete over integrated (the loader's first device is often the iGPU on
  laptops). Logged as `[vulkan] GPU: ...`. Override with `PSPRECOMP_VULKAN_DEVICE=<index or name part>`.
- **Present mode**: `PSPRECOMP_VULKAN_PRESENT_MODE=fifo|mailbox|immediate|relaxed` (default fifo).
- **Surface format**: ranks plain UNORM formats first; an sRGB surface would apply a second gamma curve
  and wash out every colour (Windows drivers often list an sRGB format first).
- A latent bug fixed: the present fence was reset *before* acquiring an image, so a failed acquire
  would leave it unsignalled and stall the next frame for 5 s.
- Win32 input now tells the game which device is in use (keyboard/mouse vs controller) so on-screen
  prompts switch ("Press F" / "Press X"), exactly like macOS.
- Async GE and parallel vertex decode are on by default **for the Vulkan backend only**.
- `display_window.cpp` split per platform (it was one file with a Win32 branch and a macOS branch).
- Windows scripts: `INSTALL_TEXTURE_PACK.bat` / `UNINSTALL_TEXTURE_PACK.bat`; `build_release.bat` warns
  when the Vulkan SDK is missing.

Everything from the earlier macOS work (see the feature table) is in shared code and needs no port.

## Feature parity table

"Shared" = lives in `host/` and applies on Windows automatically once the Vulkan backend is used.

| Feature | Where | Windows status |
|---|---|---|
| Real swapchain present (GPU-side, no readback) | Vulkan backend + `platform/*/vulkan_platform_*` | **OFF by default on Windows** until verified. `PSPRECOMP_VULKAN_SWAPCHAIN=1` to test; then change `swapchain_default_enabled()` in `vulkan_platform_win32.cpp` to `true` |
| Bloom (`[SimulateHDR]`) | Vulkan backend | See "Bloom" below |
| FXAA / 3-pass SMAA (`[Rendering] SMAA`, `[SMAA]`) | Vulkan backend | shared |
| Colour grading (`[ColorGrading]`), time-of-day grade, CAS, dither | Vulkan backend | shared |
| Sky palette (`[SkyPalette]`) | Vulkan backend + `vcs_sky_palette.hpp` | shared |
| Volumetric clouds (`[VolumetricClouds]`) | Vulkan backend (DX12 also has an older port) | shared |
| Sun-lit relief (`[ReliefShading]`, off by default), VHS (off by default) | Vulkan backend / `psp_ge.frag` | shared |
| Mip chains, anisotropic filtering | Vulkan backend | shared |
| Texture pack loader + auto upscaler + HUD replacement | `vcs_texture_replace.cpp` + Vulkan backend | shared. Install with `INSTALL_TEXTURE_PACK.bat` |
| Draw distance patch (`[DrawDistance]`) | `vcs_draw_distance_patch.cpp` | shared |
| Widescreen fix / HUD scale | `vcs_config.cpp`, `ge_renderer.cpp` | shared (desktop size query already has a `_WIN32` branch) |
| Project2DFX lights | `vcs_project2dfx*.cpp` | shared |
| Keyboard/mouse prompts + live switching | `vcs_key_prompts.cpp`, `vcs_profile.cpp`, per-platform input | shared logic; Win32 device tracking added in `display_window_win32.cpp` |
| Controller fixes (stick / camera Y) | per-platform input | XInput was already correct (its Y axis is up-positive); SDL path (macOS) was fixed |
| Loading / legal screen fit | `ge_renderer.cpp` | shared |
| Async GE (display lists on a worker thread) | `vcs_profile.cpp`, `src/runtime.cpp` | shared, on for Vulkan builds |
| Non-blocking texture uploads, batched replacement swaps | Vulkan backend | shared |
| Parallel vertex decode | `ge_renderer.cpp` | shared, on for Vulkan builds |
| Sky remap memo, cached `getenv` (`VCS_ENV`) | headers | shared |
| Launcher (settings UI, texture pack install) | macOS: `launcher/VCSLauncher.swift` | **Windows launcher to be written** (spec below) |

## Performance target and how it was measured on macOS

On an M1 MacBook Air (60 Hz), full stack (texture pack + async GE + parallel decode), busiest route:
frame time p50 16.7 ms, p99 ~21 ms, frames over 33 ms ~0.1%, none over 60 ms; about 9 of 16.7 ms used in
the heaviest areas (was ~16 ms, i.e. no headroom). The DirectX 12 build is the Windows baseline the
Vulkan build must **match or beat**; compare on the same machine and route.

How to measure (do the same on Windows):
- `PSPRECOMP_FRAME_TIME_DIAG=1` prints one `[frame-time]` line per vblank with `frame_us`, `ge_us`,
  `ge_async_wait_us`, `present_us`, `io_us`. Use the busiest 40% of a fixed route (airport and city).
- **Never run a profiler during a timing run** (it stalls threads and ruins the window it covers).
  Profile in a separate run.
- Judge frames over 20 / 33 / 60 ms and p50/p90/p99, not the average fps counter.
- Do not use `PSPRECOMP_GE_GPU_TIMESTAMP_DIAG` or `PSPRECOMP_VULKAN_SWAPCHAIN=0` for perf conclusions on macOS;
  on Windows the readback path is the thing to beat, so *do* compare `SWAPCHAIN=0` and `=1`.

## Build on Windows

1. Visual Studio 2022 (C++), CMake, and the **Vulkan SDK** (sets `VULKAN_SDK`, provides `glslc` and the
   loader import library). Without the SDK the build silently would have fallen back to DirectX 12; it now
   prints a warning.
2. `profiles\vcs\BUILD_VCS.bat` (calls `scripts\build_release.bat`). Look for the CMake line
   `VCS graphics backend: VULKAN`.
3. First compile: expect to fix small MSVC issues in the Windows-only files first
   (`platform/windows/vulkan_platform_win32.cpp`, `platform/windows/display_window_win32.cpp`).
   The shared Vulkan backend compiles on macOS/clang; MSVC may warn about things clang does not
   (`getenv` deprecation, narrowing). `ge_gpu_backend_vulkan.cpp` is included in an MSVC build for the
   first time on this branch.
4. A full MSVC rebuild of the generated AOT units is slow (it was designed around `/Ob0` for cold units,
   `/Ob3` for the hot one). This round changed **no** compiler flags, so an existing build directory only
   recompiles the host files that changed.

## Verification checklist (in this order)

1. **Boots on the default path** (readback present, `swapchain_default_enabled()` is false): game reaches
   gameplay, log shows `[vulkan] GPU: <your discrete GPU> (discrete)`. If it picked the iGPU, fix
   `pick_physical_device` or use `PSPRECOMP_VULKAN_DEVICE`.
2. **Visual parity** with macOS at the same spot: SMAA, sky palette, colour grade, clouds, HUD, radar.
   Colours must not look washed out (sRGB surface bug) or too dark.
3. **Texture pack**: run `INSTALL_TEXTURE_PACK.bat`, restart, confirm textures, HUD digits, no orange rectangle
   around a parked bike's rear light, no stalls when driving into new areas.
4. **Prompts**: on-screen text says "Press F..." with keyboard/mouse and "Press X..." after touching the pad,
   and switches back.
5. **Swapchain** (`set PSPRECOMP_VULKAN_SWAPCHAIN=1`): the window shows the game; then resize, minimise and
   restore, alt-tab, toggle fullscreen: the log prints `[swapchain] recreated for WxH`; there must be no black
   frame stuck, no crash, no frame-rate collapse. Check that recreations only happen when the window
   actually changes size (`[swapchain] recreated` spam every frame means the extent check is wrong).
6. **Performance**: swapchain 1 vs 0 vs the DirectX 12 build, same route, using the metrics above. Only then
   flip `swapchain_default_enabled()` to `true` in `vulkan_platform_win32.cpp`.
7. **High refresh / VRR displays**: FIFO on a 144 Hz panel with 60 fps content judders. Try
   `PSPRECOMP_VULKAN_PRESENT_MODE=mailbox`; if it is clearly better, make it a per-platform default.
8. **Async GE** on Windows: run a long session (missions, cutscenes, saving, loading, a shop) looking for
   hangs. It was validated on macOS only. The kill switch is `PSPRECOMP_GE_ASYNC=0`.
9. Multi-GPU laptops (Optimus/hybrid): confirm presenting from the discrete GPU works.

## Bloom (read this)

On macOS the swapchain path force-disables bloom (`create_present_swapchain`, "bloom disabled: its composite
is CPU-side only"), so the shipped `[SimulateHDR]` setting has had no effect on macOS since the swapchain
became the default, even though the v1.0.4 notes say it is on. Windows on the readback path *would* show
bloom (CPU composite), so without a fix the two platforms differ. See the section "Bloom status" at the
bottom for what was done about it.

## Windows launcher spec

The macOS launcher is `launcher/VCSLauncher.swift` (SwiftUI, ~970 lines): a settings UI that edits
`VCSNative.ini` and `ProperShaders.ini` next to the game and launches it. Build the Windows equivalent with the
same pages and the same ini keys (read the Swift file: every control is a `doc.binding("<Section>", "<Key>", ...)`
line near the top, and the `Section("...")` blocks show the layout).

Pages: **Window** (fullscreen, resolution mode/size, aspect ratio, upscale filter, integer scale, show FPS),
**Rendering** (internal resolution mode/scale/size, hardware transform, anisotropic filtering, sharpen,
depth precision, SMAA), **Quality** (bloom, colour grading, post effects from `ProperShaders.ini`: SMAA,
dither, CAS, sky palette, time-of-day grade, relief, VHS, volumetric clouds; draw distance sliders; texture pack
install/uninstall), **Controls** (keyboard, mouse and Xbox layouts, keyboard prompts toggle), **Addons**
(widescreen, Project2DFX), **Performance**. Windows-specific replacements: no Metal HUD toggle; add
GPU selection (`PSPRECOMP_VULKAN_DEVICE`), present mode, and the swapchain toggle.

Rules the launcher must keep: draw-distance sliders are capped at world 6x, LOD 6x, vehicles/NPCs 2x (higher
values made vehicles despawn). Texture pack install: the zip must contain `VCSNative-TexturePack.txt` (marker) and
only 16-hex-digit `.png` files; extract into `Textures/` next to the game; keep a manifest
(`.installed-texture-pack`) so uninstall removes only what it installed. `INSTALL_TEXTURE_PACK.bat` already does
this and can be called from the launcher.

## Config keys

`VCSNative.ini` sections: Display, Rendering, Timing, Audio, Widescreen, Project2DFX, SimulateHDR (bloom),
DrawDistance, ColorGrading, Controls (`KeyboardPrompts`), Diagnostics, Textures.
`ProperShaders.ini` sections: VolumetricClouds, SMAA, Dither, CAS, ReliefShading, VHS, TimeOfDayGrade, SkyPalette.
Both parsers (`vcs_config.cpp`) are shared, so every key works identically on Windows.

## Environment switches worth knowing

`PSPRECOMP_GE_ASYNC=0/1`, `PSPRECOMP_GE_PARALLEL_VERTEX_DECODE=0/1`, `PSPRECOMP_VULKAN_SWAPCHAIN=0/1`,
`PSPRECOMP_VULKAN_DEVICE`, `PSPRECOMP_VULKAN_PRESENT_MODE`, `PSPRECOMP_FRAME_TIME_DIAG=1`,
`PSPRECOMP_EVENT_DIAG`, `PSPRECOMP_GE_DIAG`. Environment variables are read once per call site (`VCS_ENV`).

## Known risks / unknowns

- Every Windows-only file is unbuilt. Start there.
- `ge_gpu_backend_vulkan.cpp` still contains macOS-era assumptions to re-check on desktop GPUs: transient/lazily
  allocated attachment memory (desktop GPUs may not offer it; there must be a fallback), descriptor and pool
  sizes, TBDR-oriented batch coalescing (harmless on immediate-mode GPUs but tuned for Apple), `VK_KHR_portability_*`
  (only requested when the extension exists, so Windows skips it).
- Stale header comment in `ge_gpu_backend_vulkan.cpp` claims hardware-transform paths are not implemented; the
  `psp_ge_hw*` shaders exist, so verify `Rendering.HardwareTransform` behaviour before advertising it.
- No HDR / wide-gamut output handling. Multi-monitor and fractional DPI scaling are untested.
- The macOS present uses `imageUsage = COLOR_ATTACHMENT`; some Windows drivers want extra usage flags only for
  screenshot tools, which is not required for this game.
- Frame pacing: the game limiter runs at 59.94 Hz virtual time; against a 60.00 Hz FIFO display an occasional
  repeated frame is expected. That was inherited from before this work.

## macOS verification of this round (done on the Mac before handoff)

Built with the full CMake auto-selection (macOS layer chosen automatically) and run on an M1 MacBook Air:

- Default run: boots to gameplay with the texture pack; log shows `[vulkan] GPU: Apple M1 (integrated)`,
  `[swapchain] macos surface created`, swapchain created (2880x1800, format 44, 3 images, present mode FIFO);
  no swapchain recreations during normal play; throughput unchanged versus the previous build.
- **Recreation stress**: with a temporary hook forcing a recreation every 120 presented frames, 30 recreations
  ran in 75 s with 0 failures, no hang, correct picture (hook removed afterwards).
- **Readback fallback** (`PSPRECOMP_VULKAN_SWAPCHAIN=0`, what Windows starts on): boots and renders correctly, but
  moves roughly half the frames per second of the swapchain path in the same boot window. Expect Windows to be
  slower until the swapchain is enabled there.
- DirectX 12 build: not built on the Mac. Its behaviour is preserved by construction (new defaults are gated on
  `ge_gpu_backend_is_vulkan()`, which is `false` in `ge_gpu_backend_dx12.cpp`), but this must be confirmed by
  building the DX12 configuration once (`-DPSPRECOMP_VCS_GE_BACKEND=DX12`).

## Bloom status (open)

Unchanged in this round. The plan, if approved: add a GPU composite to the present pass so bloom works on the
swapchain path on both platforms. Bloom's output is a same-size image that the CPU currently adds to the final frame
with a per-channel clamp (`min(base + glow, 255)`); `shaders/present.frag` can do the identical
`clamp(scene + glow, 0, 1)` with a second sampler, and the per-frame bloom readback copy can be skipped when the
swapchain presents. This turns the shipped `[SimulateHDR]` setting on for macOS too (a visible change), so it needs the
owner's approval.

## Branch note

The Windows-side fixes that lived only on the old `dx12-windows-backport` branch were audited against this tree:
the 64 MB guest RAM fix, the GE backend lock and the draw distance patch were already present here (they originated
from the macOS work); the cursor-confinement fix (`ClipCursor`, commit de301d1) was missing and has been ported into
`platform/windows/display_window_win32.cpp`.

## BLOCKER before the swapchain can be the Windows default: software-presented frames

Some frames are not rendered by the GPU backend: the intro videos, and any screen the game draws straight into the
guest framebuffer. `vcs_profile.cpp` sends those to `display_window_present()` / `display_window_present_rgba()`
whenever `ge_gpu_backend_presents_directly()` is false for that vblank. In `display_window_win32.cpp` those functions
present through the standalone DirectX 12 presenter (only when `backend == DirectX12 && !dx12_ge_color`), otherwise
through **GDI** (`PostMessageW(kMessagePresent)`, a `StretchDIBits` blit) into the *same HWND*.

While a Vulkan swapchain owns that window, mixing in a GDI blit or a second DXGI swapchain will fight the
presentation engine (black or flickering video, lost frames). macOS solved the equivalent problem by sharing the
`CAMetalLayer` with SDL's Metal renderer.

Suggested fix (platform-neutral, also usable by macOS): add `ge_gpu_backend_present_rgba(std::span<const std::byte>,
width, height)` to `ge_gpu_backend.hpp`; in the Vulkan backend upload the CPU frame to a texture and draw it through
the existing present pass, and make the Windows `display_window_present*` call it instead of GDI when the swapchain
is active. Until this exists, test the swapchain with intro videos and loading screens, and keep
`swapchain_default_enabled()` false on Windows if they misbehave. (It is safe to test by setting
`PSPRECOMP_VULKAN_SWAPCHAIN=1` and starting the game from the very beginning: the legal screens and intro movie are
the first frames to exercise this.)

The CPU-readback path (the Windows default for now) is unaffected: there the backend never owns the window.

## Fixed after the first push: texture-cache memory blow-up (affects every platform)

Symptom (reported by the owner on macOS with the texture pack installed): after a few minutes of play performance
degraded, then the screen flickered to permanent black while the game kept running. Root cause: the Vulkan
texture cache key is `address + format + palette checksum`, not pixels. A texture whose palette is rewritten every
frame (one 256x256 palette-indexed texture at guest address 0x08706bf0 in the opening area) produced a new key each
frame even when the decoded pixels were identical, and with the pack each copy got its own multi-megabyte replacement
image: the process reached 10-12 GB of GPU memory within about two minutes and then lost its GPU frames.

Fix (in `ge_gpu_backend_vulkan.cpp`): a new key whose pixels, size and sampler match a resident entry is **aliased** to
that entry (`texture_alias`, `texture_content`, `find_texture()`), so identical content exists once. Verified: process
memory footprint flat at about 850 MB over four minutes (was 10 GB+), about 1,000 resident textures instead of 3,000+,
GPU submissions healthy. A safety net also caps total replacement-image memory at 3 GB (`replaced_bytes`); past it
the original small texture is kept. Also fixed: a replaced entry now keeps its content signature.

**Windows must soak-test this**: run at least 5 minutes with the pack installed, idle in the opening area, and watch
the process's *GPU/committed* memory (Task Manager Details > GPU memory, or `dxdiag`/PIX). It must stay flat. On
macOS the memory shows under `vmmap -summary <pid>` as "owned unmapped (graphics)" and in "Physical footprint"; a plain
RSS number hides it. Also watch that `PSPRECOMP_FRAME_TIME_DIAG=1` frame times do not drift upward over the soak.
