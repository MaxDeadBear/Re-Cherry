# Building

## Dependencies

### Windows

- LLVM/clang (the presets call `clang` and `clang++`, not clang-cl), CMake 3.25 or newer, Ninja. Visual Studio's "C++ Clang tools for Windows" component provides all three, or `scoop install llvm cmake ninja`.
- Git

### Linux

- clang 20, CMake, Ninja, and the X11 and GTK development headers. Pass `-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20` if your distribution only has versioned names.

## 1. Get the ReXGlue SDK

Download the release matching `sdk_version` in `re_cherry_manifest.toml` from the [SDK releases](https://github.com/rexglue/rexglue-sdk/releases), unpack it anywhere, and point `REXGLUE_SDK` at the folder that contains `bin/rexglue`, `include/` and `lib/cmake/`:

```bash
export REXGLUE_SDK=/path/to/rexglue-sdk          # PowerShell: $env:REXGLUE_SDK = "C:\path\to\rexglue-sdk"
```

The presets read the SDK location from that variable. If `rexglue`'s `bin/` folder is on `PATH`, CMake finds the SDK from there as well.

## 2. Provide the game

Extract your own dump of Lollipop Chainsaw (Xbox 360) into `assets/`, so that `assets/default.xex` and `assets/AshGame/` exist. Codegen needs only `default.xex`; running the game needs the whole folder.

## 3. Recompile the game code

```bash
rexglue codegen re_cherry_manifest.toml
```

This writes `generated/default/` from the manifest and the files it includes under `config/`. After the first run, the build re-runs codegen by itself whenever one of those files changes.

## 4. Build

```bash
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
```

`release` is the distribution preset and `debug` is available for stepping through host code. In Visual Studio, open the folder and pick the preset from the configuration list.

## 5. Run

Run `out/build/relwithdebinfo/re_cherry.exe`. With no `--game_data_root`, it uses the nearest `assets/` folder holding `default.xex` at or above the executable's folder, which for a build tree is the repository's `assets/`.
