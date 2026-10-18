<img width="1287" height="832" alt="Re_cherry_logo" src="https://github.com/user-attachments/assets/ec7c9bdc-1438-44d9-9760-bfe054b9971d" />

Re-Cherry is a [ReXGlue](https://github.com/rexglue/rexglue-sdk) static recompilation of Lollipop Chainsaw (Xbox 360).

DOWNLOAD
----------------------------------------------
Beta builds are on the [Releases](https://github.com/MaxDeadBear/Re-Cherry/releases) page. Extract the zip, put your own extracted Lollipop Chainsaw (Xbox 360) ISO into its `assets\` folder (`default.xex` and the `AshGame` folder), and run `re_cherry.exe`. A GPU with Direct3D 12 or Vulkan is required.

HOW TO BUILD
----------------------------------------------
Full instructions are in [docs/building.md](docs/building.md). In short:

1. Install the ReXGlue SDK version named in `re_cherry_manifest.toml` and set `REXGLUE_SDK` to it (or put its `bin/` on your `PATH`).
2. Install clang, CMake and Ninja. On Windows, the Visual Studio "Desktop development with C++" workload with "C++ Clang tools for Windows" checked provides all three.
3. Dump your copy of Lollipop Chainsaw (Xbox 360) and extract the ISO (including `default.xex`) into `assets/`.
4. `rexglue codegen re_cherry_manifest.toml`
5. `cmake --preset relwithdebinfo` then `cmake --build --preset relwithdebinfo` (in Visual Studio: open the folder, pick the `RelWithDebInfo` preset, Build All).

HOW TO USE
----------------------------------
The game looks for an `assets/` folder holding `default.xex` next to `re_cherry.exe` or in any folder above it, so a build in `out/build/relwithdebinfo/` finds the repository's `assets/` on its own. For a standalone copy, put `re_cherry.exe` (and the DLLs beside it) in a folder together with an `assets/` folder that contains the `AshGame` folder and `default.xex`. `--game_data_root=<path>` points it anywhere else.

Graphics settings are on the game's own options screen: press Y (or LB/RB) there for two GRAPHICS pages, and every change applies at once except switching the renderer to Vulkan, which needs a restart (docs/ui/option-menu.md).

* GRAPHICS 1: resolution (144p to 2160p), anti-aliasing (FXAA), texture filtering, aspect ratio (original, stretch, 21:9, 32:9 with a hor+ camera), display mode, v-sync, frame rate (30, 60, uncapped).
* GRAPHICS 2: shadow quality, field of view, brightness, sharpening, skip intro logos, FPS counter, renderer (D3D12 or Vulkan).

Everything is also in the F4 overlay: Lollipop/Graphics, Lollipop/Performance (`target_fps` 30/60/uncapped), Lollipop/Costumes (costume slot swaps, mods go in `assets/SkinMods/`) and Lollipop/Patches (Xbox Live prompt skip, `skip_startup_logos` for the five logo movies at boot). F12, or X on the options screen, toggles the FPS counter. Settings are saved to `re_cherry.toml` next to the exe.

CURRENT ISSUES WITH THE GAME
--------------------------------------
Screen-Tearing

Re-Cherry uses its own native renderer (D3D12 or Vulkan) instead of an emulated GPU, and it is in beta. See [docs/renderer/README.md](docs/renderer/README.md).

IF YOU FIND ANY CRASHES PLEASE MAKE AN ISSUE EXPLAINING WHERE IT WAS AND WHAT YOU WERE DOING (i.e. went to high-school level, game crashed during loading or nick-toss on enemy causing the game to crash)(no these dont happen anymore they're just examples)

CREDITS
MadLadMikael - for helping setup and teaching howto use REXGLUESDK and GITHUB


DISCLAIMER: Please support the official repop release. while it may be controversial and this may be the alternative you may prefer they worked hard to bring you new content in there as well.

RE-CHERRY AND ITS DEVELOPERS DOES NOT CONDONE PIRACY.
