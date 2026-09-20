# VCSNative: Experimental Windows + macOS release

## New features
- **Windows Vulkan build**, with a new single-exe Windows launcher (`VCSLauncher.exe`) that needs no dependencies.
- **Windows launcher pages:** Window, Rendering, Quality, Controls, Addons and Performance. It edits `VCSNative.ini` and `ProperShaders.ini` in place, keeps comments and line endings, and lets you choose the GPU, present mode, swapchain and worker threads.
- **Texture pack:** install and uninstall from the Windows launcher, with validation. It asks for the PSP_DATA folder if it's missing and warns if the display refresh rate isn't a multiple of 60.
- **Swapchain presentation** on Windows through Vulkan instead of CPU readback. Videos and loading screens use it too, and the CPU readback path remains as a fallback.
- **clang-cl build** for the generated game code and the renderer (`PSPRECOMP_VCS_CLANG_AOT`, 12 shards). MSVC remains as a fallback.
- **GPU selection** on Windows: discrete GPU first, then most video memory. `PSPRECOMP_VULKAN_DEVICE` accepts an index or part of a name.
- **Windows system tuning:** 1 ms timer, above-normal priority, EcoQoS opt-out, MMCSS "Games", per-monitor-v2 DPI awareness, discrete-GPU hints for NVIDIA and AMD, a VRAM-aware texture budget, and a startup CPU-feature check with a clear message.
- **Swapchain resize:** minimise, maximise and resize are handled.
- **Raw-input mouse camera** on both platforms. Each frame's movement is converted once, proportionally, with no stick-style curve. The cursor is confined to the game window on Windows.
- **Camera options in the macOS launcher:** a new Camera section with camera on right stick, invert camera Y, mouse sensitivity (default 50) and look-up limit (default 40°). The Windows launcher has the same options.
- **Look-up limit** (`PedCameraUpLimitDegrees`): the on-foot camera tilt limit is now adjustable, with a default of 40° (the game's own stop is 45°).
- **Draw distance:** World objects and model detail switch are capped at 3× (defaults: World 3.00, LOD 3.00). Vehicles and pedestrians are always 1×, so their sliders are removed and old ini values are ignored.

## Bug fixes
- **Cutscene audio stutter and desync** on both platforms. ATRAC3+ files with 280- or 376-byte frames (all cutscene dialogue and the CITY tracks) were treated as ATRAC3, so the game consumed the stream at double speed and played silence for the second half of cutscenes.
- **HUD and field of view:** the Vulkan backend showed a 480×272 crop of the 512×320 frame, which magnified the view and HUD. It now presents the whole frame, like the original DX12 renderer.
- **HUD at every aspect ratio** (16:9, 21:9, 32:9, 4:3, 16:10, 5:4, tall): the widescreen factor is restored to the original clamp and the HUD uses the original symmetric X correction. The layout follows window resizes.
- **Windows audio drift:** if audio falls more than about 140 ms behind, the sink drops the stale part and restarts. Lagging streams snap forward with a short gap instead of drifting.
- **Sun-lit surface relief** option removed everywhere (shader, config, both launchers) because it looked broken.
- **Controllers:** a deflected stick always wins over the mouse.
- **Draw distance:** the macOS launcher shows only World objects and Model detail switch.
