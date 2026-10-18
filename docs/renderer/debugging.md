# Debugging the renderer

## `--cherry_debug_hang_dump=true`

Arms three things (`src/app/debug/hang_watchdog.cpp`, `src/engine/debug/render_thread_trace.cpp`):

* **Hang dump.** If no frame is presented for 8 seconds, every thread is suspended in turn and its host stack is logged under `[hang]`. Recompiled guest functions appear as `__imp__sub_XXXXXXXX` (or their `config/` names), so the guest call chain can be read straight off the host stack. A guest tail call compiles to a host sibling call, so the function that made it is missing from the chain.
* **Swallowed exceptions.** A vectored handler registered after the SDK's MMIO handler logs the first 16 hardware exceptions and traps under `[exception]`, with the faulting address and stack. UE3's own `__try` blocks catch these silently.
* **Rendering-thread lifecycle.** `StartRenderingThread`, `StopRenderingThread`, `FRenderingThread::Run` and `RenderingThreadMain` log under `[rt]`. UE3 stops and restarts its rendering thread twice during startup; that is normal.

lldb from LLVM needs `python311.dll` to start on Windows, which is why the stacks are taken in-process.

## `--cherry_debug_screenshot_every=N`

Copies every Nth presented frame out of the swap chain into `shots/frame_NNNNNN.tga` in the working directory (`src/gpu/debug/screenshot.cpp`). It captures exactly what the renderer drew, never anything else on screen. plume's D3D12 `copyTextureRegion` dereferences the destination's texture even for a buffer footprint, and its Vulkan one has no image-to-buffer path; `CopyToReadback` works around both as Kameo does.

## `--cherry_debug_trace_frame=N`

Logs every command of replayed frame N under `[trace]`: draws with their shaders, targets and render state, each bound texture with its fetch and shader dimension and the heap index it got, clears, and resolves with their rectangles, destination and volume slice.

## `--cherry_debug_frame_timing=true`

Logs every 150 presents the average milliseconds per frame spent capturing draws, preparing textures, recording the replay, waiting on the GPU and submitting; the rest is the guest and the `target_fps` pacing (`src/gpu/debug/frame_timing.cpp`). Leave screenshots off while measuring, since each one waits for the GPU and writes a file.

## `--cherry_debug_edram_alias=true`, `--cherry_debug_bad_vertices=true`, `--cherry_debug_late_vertex_writes=true`

Investigation switches in `src/gpu/debug/`:
* `edram_alias` follows which surface last wrote each EDRAM tile, and logs every 150 frames the draws and resolves that read tiles another surface (format, size or base) wrote.
* `bad_vertices` logs draws whose decoded positions are NaN, infinite, denormal or beyond 1e7, and says where their vertices were read from.
* `late_vertex_writes` logs draws whose vertex or index bytes the guest changed after the draw call.

## `--cherry_debug_press=frame:button[+button],...`

Presses buttons on pad 0 at given presented frames, each held for 6 frames (`src/engine/debug/scripted_input.cpp`), so a run can get past the title without a controller: `--cherry_debug_press=1000:start,1300:a`. While it is set, pad 0 reads as connected even with no controller.

## `--cherry_debug_profile_frame=N`

From presented frame N, samples every thread's instruction pointer and call stack about once a millisecond for `--cherry_debug_profile_seconds` (default 10), then logs each busy thread's 25 hottest functions, self and inclusive (`src/app/debug/sampling_profiler.cpp`). Recompiled guest functions show as `__imp__sub_XXXXXXXX`; look them up in the IDA database. Combined with the input script, it profiles gameplay without a controller: `--target_fps=30 --cherry_debug_press=1000:start,1300:start,1600:start,1900:a,2200:a,2500:a,2800:a,3100:a --cherry_debug_profile_frame=4600 --cherry_debug_frame_timing=true` reaches the school gate. Keep the frame rate fixed for such runs, since the presses are counted in frames.

## `--cherry_debug_dred=true`

Turns on D3D12 DRED before the device is created (`src/gpu/debug/dred.cpp`). When the device is removed, the renderer logs the reason and, under `[dred]`, the operations around the last one the GPU finished in each unfinished command list, how many draws came before it, and the page fault's address with the allocations that were live or recently freed there.

## RenderDoc

RenderDoc (`C:\Program Files\RenderDoc`) captures the native renderer's D3D12 or Vulkan frames. `src/gpu/debug/renderdoc.cpp` triggers captures from inside the game, so a given presented frame can be captured without a key press:

```powershell
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture -c F:\Re-Cherry\rdcaps\cherry -d out\build\relwithdebinfo out\build\relwithdebinfo\re_cherry.exe --cherry_debug_renderdoc_frames=1500,1800
```

`--cherry_debug_renderdoc_frames` lists presented frame numbers; the game arms `TriggerCapture` one present early. RenderDoc's own capture key (F12 / Print Screen) still works and adds captures of its own. Captures land in `rdcaps/` (ignored by git).

`tools/renderdoc/dump_capture.py` reads a capture headless, through the Python that ships inside `qrenderdoc.exe` (there is no standalone module on Windows):

```powershell
$env:CHERRY_RDC = "F:\Re-Cherry\rdcaps\cherry_frame1800.rdc"; $env:CHERRY_RDC_OUT = "<folder>"; $env:CHERRY_RDC_DRAW_EVERY = "60"
& "C:\Program Files\RenderDoc\qrenderdoc.exe" --python tools\renderdoc\dump_capture.py
```

It writes `actions.txt` (every API action with its outputs), a PNG of every copy destination (resolves, and texture uploads), the swap chain at the end of the frame, and with `CHERRY_RDC_DRAW_EVERY` the colour target after every nth draw. Float targets above 1.0 save as white.

## Problems found so far

| symptom | cause | fix |
| --- | --- | --- |
| `std::_Destroy_range<DeviceInfo>` access violation under `InputSystem::RefreshDevices` | The SDK rebuilds its input device list without a lock, and UE3 polls input from both its game and rendering threads | The guest's three XInput wrappers are serialised (`src/engine/xinput_lock.cpp`) |
| Point-list draws with integer positions every frame during loads | UE3's texture-pool defragmenter moves memory by drawing points with memory export (`0x82986B18`) | `XeTexturePool_GpuMemmove` is a native `memmove` (`src/gpu/hooks/memexport.cpp`) |
| The render thread stops inside `ExpandPackedToRgba8`; the unwind ends in `_NLG_Return2` | Its last argument is the size of one packed texel, and the whole buffer's size was passed, so each texel copied the buffer into a 4-byte local | Fixed in `texture_cache.cpp`; the header now says what the argument is |
| The 3D scene shades black under a white sky; HDR and depth textures fall back to the placeholder | Resolves registered the destination under the texture object's virtual address (`0xF37BF000`) while draws sampled the physical one SetTexture wrote (`0x137C0000`); depth resolves were not done at all | `TextureFetch::address()` canonicalises to physical; depth resolves draw through `depth_resolve.hlsl` |
| Most scene draws dropped "without shader" once gameplay starts | Shaders the game creates later are not in the cache yet; they are dumped as they miss | Rebuild the cache (`python tools/shaders/build_shader_cache.py`) and the game |
| Stall about 2 s in, right after `Textures.xxx` opens; the game thread waits in `FFrameEndSync::Sync` (`sub_829BB7E0`) | The rendering thread spins in `D3DQuery_GetData` under UE3's occlusion fetch `sub_829802E0`: the query's ZPASS counters stay at the GPU's `-275` sentinel forever | `D3DQuery_GetData` is a `REX_HOOK` (`src/gpu/hooks/query.cpp`) that reports occlusion queries as visible until the draw path runs real ones |
| The title composite turns white | UE3's colour-grading LUT is a 16x16 stack of 16 slices (fetch dimension 2D, stack depth in bits 26..31 of dword 2), assembled by 16 resolves selected by `DestSliceOrFace` and sampled with `tfetch3D` | Resolves keep the slice; a stacked destination is a host 3D texture in the 3D heap |
| The title looks white in screenshots | The swap chain's alpha is 0 where the game left it, and viewers composite the 32-bit TGA over white | Screenshots are written as 24-bit TGA |
| Four draws a frame dropped "without shader" | They bind no pixel shader at all: depth-only shadow and pre-pass draws | A null pixel shader replays depth-only; only an unknown one is dropped |
| Access violation in `D3D12CommandList::setPipeline` | plume ignores `CreateGraphicsPipelineState`'s result and returns a pipeline with a null PSO | `DrawPipeline` drops a D3D12 pipeline without a PSO and logs its state |
| `887A0006` (device hung) shortly after Start | DRED: a page fault in a depth resolve on a texture already freed. One guest address is resolved at several sizes, and each change recreated the texture while the frame in flight still used it | Resolve textures are keyed by address and size; replaced ones are released three frames later |
| The 3D world is black in gameplay while the HUD draws; RenderDoc shows the depth buffer uniformly about 0.999 | UE3 reverses depth through the viewport (`VportZScale` -1, `VportZOffset` 1, depth cleared to 0, test greater-or-equal), and replay sorted the host viewport's min and max depth, so a far surface at 0.999 won every test | The host viewport keeps min > max, and `GPU_VTECONTROL`'s Z enable bits decide whether the scale and offset apply |
| Title logo, menu panels and HUD pieces drawn at the wrong place or as whole atlas pages | Vertex components decoded without the declaration type's selectors, and 16-bit pairs in the wrong order; UE3 also binds declarations through `D3DVertexShader_Bind` rather than the device | Vertices follow Bind's patcher (`sub_829EC9C0`): number format bits 8..9, selectors bits 10..21, XOR 1 for 16-bit types; the declaration comes from a native Bind hook |
| Gameplay crushed to black with blown-out highlights; RenderDoc shows the HDR scene lit correctly | Resolves were plain copies, but `GPU_COPYDESTINFO.CopyDestExpBias` (Resolve flags bits 26..31) scales the copy by a power of two, on top of the source's `GPU_COLORINFO.ColorExpBias`; the scene is resolved at 2^-3 and nothing scales it back on fetch, so the tonemap saw 8x too much light | A resolve whose combined bias is not zero draws through `color_resolve.hlsl` with the scale; colour targets now have a bindless descriptor |
| `--cherry_debug_verify_d3d` reported garbage from the game's own XG functions | A typed `REX_IMPORT` call builds its frame from ThreadState's saved r1, which is stale inside a typed hook, so the callee's frames land on live guest frames | Guest calls from typed hooks go through `cherry::core::CallGuest` (`src/core/guest_call.h`), which runs them on a per-thread scratch stack, one region per nesting level |
| Skinned meshes (characters, the chainsaw) had vertices pulled far from their neighbours, as long streaks | Their positions are `D3DDECLTYPE_HEND3N` (0x2A2190, `GPUVERTEXFORMAT_10_11_11`), and 10_11_11 and 11_11_10 had their field widths swapped; the format names list fields from the high bits down | Format 16 decodes X:11, Y:11, Z:10 from bit 0, format 17 the reverse |
| About 4 fps in gameplay; `--cherry_debug_frame_timing` showed capture and textures taking most of the frame | Every draw's vertices were decoded and uploaded again each frame (189 MB a frame), and a texture was hashed on every use rather than once a frame | Converted geometry is cached per draw and kept in its own GPU buffers while an XXH3 of the guest bytes it read is unchanged; a draw whose bytes change falls back to the per-frame path. Textures check the frame before hashing |
| Cutscenes washed out; every screen-aligned pass one pixel short on the right and bottom (the colour-grading LUT slices were 15 pixels wide) | The game's D3D sets `GPU_VTXCONTROL.PixCenter` to `GPUPIXCENTER_ZERO` (`sub_82A00E20`), so guest pixel centres are on integer coordinates, half a pixel before the host's | The host viewport moves by +0.5 pixels while PixCenter is 0 |
| A pass that writes render target 1 (`sub_82783FF0`) left its resolve with stale contents | Only render target 0 was bound | Draws and clears bind render targets 0..3 with their own blend controls and colour mask bits |
| Black and solid-colour slabs in effects, cutscenes worst; RenderDoc traced them to vertex shader inputs that were integers, NaN, or another draw's data | Particle, beam and instancing shaders compute the index they fetch at from the vertex index in r0.x, and the game writes one vertex per particle for four drawn; the translator started every shader with r0 = 0 and fetched the current vertex | `r0.x` starts as the vertex index, and fetches at a computed index read the vertex atlas (shaders.md) |
| `Begin*Vertices` draws read before the guest filled them, with indices shifted after the first one out of range | The data was read at the next D3D call, `BaseVertexIndex` and the index format's endian were ignored, and an out-of-range index was dropped alone | Read at the frame's end from the per-frame sink; the base is added, the endian comes from the `D3DFORMAT`, and a primitive with an out-of-range index is dropped whole |
| Depth of field and bloom read an empty buffer | UE3 downsamples the scene by resolving a 4x MSAA surface laid over its EDRAM tiles; the host gave that surface its own empty texture | A resolve from an MSAA surface reads the single-sample target over the same tiles, if that was written more recently, and averages each block of samples (`color_resolve.hlsl`) |
| White-outs and red or black slabs in bright scenes; the scene buffer held -1799..4584 | The 7e3 scene target clamps on the console to [0, 31.875], and the host's 16-bit floats do not | Pixel shaders clamp their colour outputs to the bound target's format range |
| Garbage vertices flying around whenever gore is on screen | The SubUV sprite shader (`8EE4A8916571C0E7`, 88-byte particles) overwrites `r0.x` with the particle index before fetching, and a fetch indexed by `r0.x` was taken to mean the current vertex | The translator tracks writes to `r0.x`; a fetch after one reads the vertex atlas |
| The sky and the distance go black in one cutscene shot (Juliet's leap and landing) | The depth-of-field downsample writes linear depth times a signed scale into alpha, about -238 for the sky, and the translated shaders' alpha test used -1 as the pass-all threshold, so those pixels were discarded and depth of field blended the cleared black over everything far | Pass-all is -infinity; `--cherry_debug_renderdoc_on_shader=<hash>` captured the shot, whose cutscene timing defeats frame numbers |
| Stutter in gameplay; slow frames of 40 to 48 ms on a 33 ms budget | The renderer's 8 to 15 ms ran after the guest's frame on the same thread, presents drifted with that work, and the limiter slept a whole extra frame after any long one | Frames are replayed and presented on a render thread, paced right before the present; a late frame starts a fresh schedule. With vsync on D3D12 the render thread waits for the output's vblank and presents every N refreshes, N the whole number nearest refresh / target_fps (measured from `WaitForVBlank`, since plume reports no D3D12 refresh rate), so every frame stays up equally long: a 100 Hz display gets 33.3 fps rather than 30 alternating three and four refreshes. The game advances by elapsed time, so its speed does not change |
| The whole image too bright and washed out next to the compare window | Textures fetched with `GPUSIGN_GAMMA` were sampled as linear, so every lit surface came out with its gamma-encoded colour | Gamma components are decoded on upload: block-compressed textures take their sRGB format, RGBA8 and the formats expanded to it become linear RGBA16 |
| Blood decals and the shadows on the bus and cars flicker | UE3 draws decals and shadow depth with depth bias, and the replay ignored it, so they z-fought | The slope term goes to the pipeline (the guest's x16 value / 16); the constant term is window depth, which a host float depth buffer cannot take as an integer bias, so the vertex shader adds it in NDC (`g_DepthOffset`, offset / viewport depth scale) |
| Whole triangles on distant cars and walls flicker between lit and dark | UE3 lays depth with a depth-only vertex shader, then draws colour with another shader and a greater-or-equal test; the console rounds both to the same depth, but the host compiler fused and reordered each shader's position math differently (RenderDoc: 0.007480628 laid, 0.007480593 drawn), so triangles failed the test at random | `oPos` is declared `precise`, so no position math is fused or reordered |
| Characters or objects drawn as solid black silhouettes (Juliet in the costume menu, a van) | Their pixel shader is not in the translated cache, so the pipeline has no pixel shader; RenderDoc shows `ps None` on the draw | Material shaders sit in compressed UE3 packages and only GlobalShaderCache-Xbox360.bin is readable, so the cache learns them as the game creates them: misses are dumped to `shader_dump/`, then `python tools/shaders/build_shader_cache.py` and rebuild |
| Shaders `0F2A5769E3927AFD` and `5E1817778BF2E9BB` failed to translate (`s16_Texture2DDescriptorIndex` undeclared), so their draws were dropped | They are vertex shaders fetching a texture from fetch constant 16, and only pixel samplers 0..15 were declared and captured | Samplers 16..19 are captured, resolved as 2D textures and declared at shared constants +336/+352; vertex shaders sample the base level |
| Memory and texture slots grow with every area visited, and after 16384 distinct textures new ones come back missing | The texture cache kept every texture and descriptor slot it ever created; UE3 streams textures in and out per level | Textures no frame has fetched for 1800 frames are evicted, freed three frames later, and their heap slots reused (`TextureCache::EvictUnused`); a 3-minute title-screen run holds private, GPU memory and handles flat |
| Pure red specks in dark, shadowed areas above 720 lines | Every render target was scaled, the 16x16 colour-grading LUT slices too; the LUT pass computes each texel's input colour from `vPos`, which at 3x became fractional, so the edge texels graded slightly negative inputs into red. The tonemap's dither noise pushes dark pixels onto that edge. RenderDoc: the uberpost pixel shader's 3D LUT sample returned (1, 0.019, 0.0007) at coordinates (1/32, 0.07, 0.095) | Targets smaller than 32 pixels on a side stay at 1x (`TargetScale`), and fetch offsets on scaled resolves use the resolves' own scale (`g_ScaledTextures` bits 16..31) instead of the drawn target's |
| Gameplay frames spent about 15% of the render thread creating textures | UE3 resolves the scene to one 1280x720 address twice a frame, once through `color_resolve.hlsl` (a render target) and once by copy (a plain texture), and the destination was recreated, 66 MB at 4K, every time the kind switched | 2D resolve destinations are always created as render targets, with the framebuffer made on first use |
| 30 FPS ran at 37.5 or 25 with V-SYNC on a 75 Hz display, from run to run | The vsync pacer presented every round(75 / 30) refreshes, and 2.5 rounds either way with the measured rate | Each frame is due 1/fps after the previous one's due time and takes the first vblank from half a refresh before it, so 2 and 3 refreshes alternate |
