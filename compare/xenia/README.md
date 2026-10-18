# Xenia side-by-side comparison mode

A debug-only build in which one game process feeds two renderers at once:

* **Left window**: the SDK's Xenia GPU emulation (`rexgpu-xenos.dll`, loaded as the runtime's GPU plugin). The game's original D3D code runs and writes its ring buffer, and Xenia draws from it.
* **Right window**: Re-Cherry's native renderer, replaying the same draws, clears, resolves and swaps.

Both windows show the same frame of the same game, so visual differences can be spotted directly. Xenia is only something to look at here. Fixes are still worked out from the game itself (IDA, RenderDoc), never from Xenia's code.

## Isolation

Everything lives in this folder. Outside it there are only:

| file | what |
| --- | --- |
| `CMakeLists.txt` | `include(compare/xenia/compare.cmake)` and `cherry_xenia_compare_target(re_cherry)` |
| `CMakePresets.json` | the `compare` configure and build presets |
| `src/app/app.cpp` | three `#ifdef CHERRY_XENIA_COMPARE` hand-offs: `ConfigureRuntime`, `ArrangeWindows`, `Shutdown` |
| `src/gpu/debug/frame_capture.cpp`, `GraphicsSystem::CaptureNextFrame` | the native half of a paired screenshot; Xenia-free, and unused outside this mode |

The option `CHERRY_XENIA_COMPARE` is off by default, so the normal build compiles none of this, links no Xenia code and loads no plugin. Nothing in this mode edits the SDK: the plugin DLL is copied unchanged from the SDK's install.

To remove the mode, delete this folder and the lines in the table above.

## Build and run

```
cmake --preset compare -Drexglue_DIR=F:/rexglue-sdk/out/install/win-amd64/lib/cmake/rexglue
cmake --build out/build/compare
out/build/compare/re_cherry.exe
```

The game's window leaves fullscreen and takes the left half of the screen, and the native renderer's window takes the right half. Input goes to the game window. Closing the native window only hides it.

All of the normal `--cherry_debug_*` options work for the native side, for example `--cherry_debug_trace_frame`.

## Paired screenshots

Press **F8** in either window. The next guest swap is written to `shots/compare_<date>_<time>_<swap>.png`, next to the executable's working directory: Xenia's image on the left, the native renderer's on the right, both at the game's front buffer size (1280x720).

Both halves are the same frame:

* The native half is that swap's front buffer, drawn 1:1 into its own target on the render thread (`src/gpu/debug/frame_capture.cpp`), so window size and letterboxing do not affect it.
* The Xenia half is the presenter's guest output (`Presenter::CaptureGuestOutput`), taken after the hook calls the game's own `D3DDevice_BlockUntilIdle` (0x829EA078). That waits on the fence D3D inserts after the swap packet, so Xenia has drawn the frame by then.
* The game is held for that one frame; the PNG is encoded afterwards on another thread.

Launched under RenderDoc, F8 also records that frame as a RenderDoc capture of the native renderer's device only (Xenia's device in the same process is left out):

```
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture -c F:\Re-Cherry\rdcaps\compare -d out\build\compare out\build\compare\re_cherry.exe
```

The game window's key is the SDK keybind `bind_compare_capture` and can be rebound in the settings overlay (F4). The native window always uses F8.

## How it differs from the normal build

* `src/gpu/hooks/` is left out: no native D3D functions, ring buffer, fences or EDRAM allocator. The game's own D3D does all of that against Xenia's GPU.
* `passthrough_hooks.cpp` hooks only these, captures each call for the native renderer, then runs the original with the guest's untouched register context:
  * DrawVertices, DrawIndexedVertices, Begin(Indexed)Vertices
  * ClearF, Resolve, Swap (which also takes paired screenshots)
  * D3DVertexShader_Bind, XGRegisterVertexShader, XGRegisterPixelShader

  They are `REX_HOOK_RAW`, unlike the main build's typed `REX_HOOK`s, so the stack arguments and `f1` pass through exactly.
* So this mode compares the native *renderer* (decoding, shaders, render targets, resolves, post-processing). It does not exercise the native D3D library replacements in `src/gpu/hooks/`, which the normal build relies on.

## Known limits

* Windows only.
* Texture contents in the native window are unreliable. Xenia's GPU writes guest memory itself (resolves, and the copies UE3's texture streaming and pool defragmenter make), and the native side reads that memory at its own time, so it can upload a texture mid-move: wrong or normal-map-looking textures on characters, broken caption boxes, and tens of MB re-uploaded every frame. The normal build does not show this. Use this mode for lighting, brightness, geometry and post-processing; confirm texture problems in the normal build.
* `Begin*Vertices` data is read at the end of the frame from the game's D3D ring data, which Xenia's GPU may already have let the game reuse. A dynamic sprite can then look different in the native window only.
* UE3's texture-pool defragmenter draws memory-export points every frame during loads. The native window replays them as ordinary draws.
* Frame pacing is Xenia's: the native window shows whatever frame the game has just swapped.
