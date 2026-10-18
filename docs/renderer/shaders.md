# Shaders

The game's shaders are Xenos microcode in v0x11 containers (magic `0x102A11xx`: `0x102A1100` pixel, `0x102A1101` vertex, plus one `0x102A1111` vertex variant), the same format Sonic Unleashed uses and upstream XenosRecomp was written for. They are translated offline by [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (the `thirdparty/XenosRecomp` submodule, patched with `tools/xenosrecomp/cherry.patch`) into HLSL, compiled to DXIL and SPIR-V, and embedded in `res/shader_cache/shader_cache.cpp`. CMake links that file when it exists and `shader_cache_empty.cpp` otherwise.

## Where the shaders come from

UE3 loads its shaders from cooked packages at runtime, so scanning the XEX finds nothing useful; the corpus is collected by playing. UE3 inlines `CreateVertexShader` / `CreatePixelShader` into `XeVertexShader_Create` (0x82982768) and `XePixelShader_Create` (0x82982950), which take the authored container in `r4`. Both are hooked in `src/gpu/hooks/shaders.cpp`. `RegisterShader` (`src/gpu/shaders/shader_registry.cpp`) hashes the container and binds the cache entry to the D3D shader object at `this+8`:

| field | where |
| --- | --- |
| container base | `r4` |
| `virtualSize` / `physicalSize` | `container[1]` / `container[2]` |
| cache key | `XXH3_64bits(container, virtualSize + physicalSize)` over the raw big-endian guest bytes |

That is exactly how XenosRecomp discovers and hashes a container, so the dump's file name, the `.meta` hash and the runtime key are the same number. The cache generator checks this for every dump.

A container that misses the cache is written to `<assets parent>/shader_dump/<HASH>.bin` (`SetShaderDumpDirectory` in `src/app/app.cpp`), so a dev build that runs from the repository dumps straight into `<repo>/shader_dump`. `--cherry_gfx_dump_shaders=true` dumps every container, hit or miss; a full regeneration needs it, because once a cache exists almost nothing misses. Dumps from different runs merge without conflicts. A shader that was never dumped has no translation.

**No container carries a D3DX constant table.** All 241 dumped so far have `constantTableOffset == 0`, so constants, samplers and bools are only known by register number (see the register-file fallback below). 175 have a definition table, which bakes literals into `c252..c255` (a few also into `c240..c251`) and the loop counts `i30` / `i31`.

## Building the translator

```bash
git submodule update --init --recursive
git -C thirdparty/XenosRecomp apply ../../tools/xenosrecomp/cherry.patch
cmake -S thirdparty/XenosRecomp -B out/xenosrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/xenosrecomp --target XenosRecomp
```

The submodule stays pinned at upstream 990d03b with the patch applied to its working tree. The cache generator reads the patched `thirdparty/XenosRecomp/XenosRecomp/shader_common.h`, so keep the patch applied whenever you build the cache. After changing the tree, regenerate the patch with `git -C thirdparty/XenosRecomp diff` and keep the header comment. `thirdparty/XenosRecomp/thirdparty/dxc-bin` ships DXC, so no SDK is needed.

## Regenerating the cache

```bash
python tools/shaders/build_shader_cache.py            # shader_dump/ -> res/shader_cache/shader_cache.cpp
```

Then rebuild the game; CMake picks the file up on the next configure. Useful options: `--containers DIR` (default `<repo>/shader_dump`), `--image FILE` (also scan any blob for `0x102A11xx` containers), `--jobs`, `--retries` (default 8), `--no-cache`, and `--keep`, which keeps the scratch directory with every shader's HLSL. Expected result: every dumped shader compiles (`compiled N, failed 0, salvaged dxil-only 0`, N growing with the dump). Only `assets/AshGame/CookedXbox360/GlobalShaderCache-Xbox360.bin` holds readable containers (`--image` finds its 325, all already dumped); material shaders are in compressed packages and reach the cache only through runtime dumps. A cold build takes about 10 s.

What the script does:

* **One process per shader, with retries.** Each container runs through `XenosRecomp.exe <bin> <out> shader_common.h --blobs`, which writes the HLSL to `<out>` and `<out>.dxil`, `<out>.spirv` (smol-v, debug info stripped) and `<out>.meta` (`hash dxilSize spirvSize specConstantsMask textureDimensionMask constantRegisterCount`). In Kameo, driving DXC hundreds of times in one process corrupted the heap; isolation plus retries absorbs that. The patched directory batch does also finish on this corpus (241 of 241, sequential, failures counted instead of asserting), but the script does not rely on it.
* **DXIL salvage.** If only SPIR-V fails, `--blobs` also writes `<out>.dxil.meta`, and after the retries run out the shader is kept with `spirvSize` 0. It then draws on D3D12 only.
* **A build cache** in `out/shader_build_cache`, keyed by the SHA-256 of each container and discarded whenever the size or mtime of `XenosRecomp.exe` or `shader_common.h` changes (`stamp.txt`).
* **Output.** `{ hash, dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask, textureDimensionMask, constantRegisterCount }` sorted by hash (the runtime uses `lower_bound`), then the two blobs. Every entry's DXIL, and its SPIR-V as smol-v, is its own zstd frame (level 19; the `zstandard` module is required, `python -m pip install --user zstandard`), compressed with a 256 KB dictionary trained on every eighth shader of that kind so small frames compress nearly as well as one stream; the offsets and sizes are of those frames, and the dictionaries follow the blobs. The runtime decompresses a shader only when the game creates it and drops the bytes once the backend has its copy, so of 21,536 shaders only the few thousand a session uses ever exist uncompressed. The declarations are in `src/gpu/shaders/shader_cache.h`.

To read one shader's HLSL, or to see its DXC error, run the following. Use absolute paths; a path that resolves from the wrong directory gives exit code 127.

```bash
out/xenosrecomp/XenosRecomp/XenosRecomp.exe shader_dump/<HASH>.bin out.hlsl thirdparty/XenosRecomp/XenosRecomp/shader_common.h [--blobs]
```

## What the translated shaders expect from the renderer

* **Constants are read by register number.** The vertex and pixel blocks are 256 float4 each, mirroring the guest device's shadows at `+0x780` and `+0x1780`. With no reflection, DXIL declares `float4 g_Constants[256]` and `#define cN g_Constants[N]`; SPIR-V uses `vk::RawBufferLoad` at `N * 16`. Registers that the definition table bakes are shader locals and are not read from the buffer.
* **Samplers** are `sN_Texture{2D,3D,Cube}DescriptorIndex` / `sN_SamplerDescriptorIndex`, read from `SharedConstants` at upstream's offsets. `textureDimensionMask` gives 2 bits per fetch-constant slot below 16 (0 = 2D, 1 = 3D, 2 = cube), saying which heap a slot is sampled from. 36 of the current shaders sample 3D textures.
* **Bools** are bits of `g_Booleans`: vertex `b0..b15` are bits 0..15 and pixel `b0..b15` are bits 16..31. Pixel shaders address their bools as `b128+N` in the microcode (Xenos has 256 bool constants, and the pixel stage gets the upper 128).
* **Spec constants.** DXIL is never compiled as a `lib_6_3` library, which D3D12 cannot put in a graphics PSO. `g_SpecConstants()` is baked to `SPEC_CONSTANT_ALPHA_TEST`, so the renderer folds the alpha function into `g_AlphaThreshold` (-infinity when disabled or ALWAYS: UE3's depth-of-field downsample writes a signed blur amount of hundreds into alpha). The SPIR-V still has upstream's spec constant 0, so pass `SPEC_CONSTANT_ALPHA_TEST` (2), **not** the entry's `specConstantsMask`. Bit 0 of that mask (`R11G11B10_NORMAL`) only records that the shader fetches a NORMAL/TANGENT/BINORMAL. UE3's Xenon RHI declares `VET_PackedNormal` as `D3DDECLTYPE_UBYTE4` (the element-type switch in `sub_829870E8`), which is neither Unleashed's 11:11:10 nor DEC3N. The decode therefore passes the raw bits through `asfloat`, and the host binds those elements with a float-producing format such as `R8G8B8A8_USCALED`.
* **r0.x is the vertex index.** A Xenos vertex shader starts with it there; UE3's particle, beam and instancing shaders read it before writing r0. Every translated vertex shader takes `SV_VertexID` and starts with `r0.x = SV_VertexID + g_VertexIndexBase`, the guest index of the draw's first host vertex.
* **Indexed vertex fetch.** A `vfetch` reads another vertex when its index register is not `r0.x`, or is `r0.x` after an earlier instruction wrote it (seven shaders). The sprites compute `r0.y = r0.x / 4`, and the SubUV sprites overwrite `r0.x` with it, then fetch one vertex per particle. Those fetches become `VFETCH_INDEXED(location, index, rounded)`, which loads the element from the vertex atlas, a 4096-wide R32G32B32A32 texture in the 2D heap (`g_VertexAtlas`, `g_VertexAtlasBase`, `g_VertexElements`, and `g_VertexSlotWord` for the element slot of each input location). Such a shader has `SPEC_CONSTANT_INDEXED_VERTEX_FETCH` (bit 6) in its `specConstantsMask`, and the renderer copies its draw's decoded vertices into the atlas.
* **Vertex texture fetch reads samplers 16..19.** `D3DDevice_SetTexture` puts sampler N in fetch constant N, so a vertex shader's `tfetch` from constant 16..19 is the texture bound to sampler 16..19. Their 2D texture and sampler indices sit in the shared constants at +336 and +352 (c21, c22), and vertex shaders sample the base level (`SAMPLE_2D`), having no derivatives.
* **`oPos` is `precise`.** UE3's depth-only and colour passes use different vertex shaders and an equal-or-closer depth test, so their positions must round identically, as they do on the console.
* **Vertex shaders add the draw's depth bias** (`oPos.z += g_DepthOffset * oPos.w`, shared constants +296). It is D3D's `DepthBias`, a window-depth offset, divided by the viewport's depth scale; the slope term stays in the pipeline.
* **Colour outputs are clamped to the EDRAM format** (`colorClamp`, two bits per target in `g_ColorClamp`). `2_10_10_10` is [0, 1] and `2_10_10_10_FLOAT` (7e3) is [0, 31.875] with a two-bit alpha, while the host uses 16-bit floats for both. Without the clamp, the negative and very large values shaders write survive into the blur and tonemap passes.
* **Vertex input locations (Vulkan)** follow upstream's table plus `TEXCOORD8` = 16 and `POSITION2..7` = 17..22. Any element the table does not list gets 23, 24, … in declaration order.

## The patch, against Kameo's

`cherry.patch` ports the generic half of Kameo's `kameo.patch`, which targets the same upstream commit (item numbers from Kameo's `docs/reverse-engineering/xenosrecomp-patches.md`), and keeps upstream's v0x11 container handling.

| Kameo item | here | why |
| --- | --- | --- |
| main.cpp: v0x0E discovery (`0x102A0E00`, `shaderOffset` test) | dropped | upstream's `0x102A11xx` + `field1C == field20 == 0` test is right for this format and is shared by batch and `--blobs` |
| main.cpp: sequential batch, counted failures, no library DXIL, `--blobs` | ported | `--blobs` also scans for the first container, writes the HLSL, writes `.dxil.meta` on a SPIR-V-only failure, and puts `textureDimensionMask` into batch output |
| pch.h `_byteswap_*` under MSVC | ported | harmless under clang |
| 1. 256 float4 registers on both stages | ported | device keeps VS / PS / bool constants at `+0x780` / `+0x1780` / `+0x2780` |
| 2. 64 temporaries | ported | one shader uses `r35` / `r36`; stock XenosRecomp failed the whole batch on it |
| 3. `tfetch1D` as 2D, y = 0 | ported | no 1D fetch in the current dump |
| 4. v0x0E `Shader` / definition-table layout | dropped | v0x11 uses upstream's structs |
| 5. `kBakeDefinitionTable = false` | dropped | v0x11 literals live in the container's physical part and are baked as upstream does. Baking is self-contained whether or not `SetPixelShader`'s register-table replay covers them |
| 6. register-file fallback | **ported, adapted** | needed: no container has a constant table. Baked registers are excluded from the declarations, because a `#define cN` would rewrite the local `float4 cN = ...`. Pixel bools also get `b128..b143` |
| 6a. v0x0E ucode pointer / length | dropped | upstream's `physicalOffset` and `size` are correct for v0x11 |
| 7 (a)-(d). relative constant addressing, `cRel`, DXIL defines after every cbuffer, `const1Relative` | ported | `cRel(i)` first selects baked literals by index, then reads the buffer. No current shader uses relative addressing |
| 8. vector/scalar co-issue (`sp0` / `sa0`) | ported, **plus a fix** | scalar `SETP*` must compute `ps` from the new predicate. Once `p0 = sp0` retires after the vector writeback, `ps = p0 ? ...` read the old one, so it reads `sp0`. Kameo's patch still has this bug |
| 9. DEC3N W = 1 | ported, not selected | the branch exists, but `DEC3N_NORMAL` is not baked; see spec constants above |
| 10. `TEXCOORD8` location | ported | plus `POSITION2..7` and the fallback, for two morph shaders DXC rejected |
| 11. `tfetch2DUnnorm` | ported | no unnormalised fetch in the current dump |
| 12. texture dimension mask | ported | the fifth `.meta` field and the seventh cache field |

Additions of our own, which are generic translator bugs:

* **Backward `CondJmp` with `direction` clear.** 32 of the 241 shaders loop backwards (for example a predicated jump from CF 9 back to 4), and the compiler never sets `direction`. Upstream trusts that bit. Where a shader also had an unconditional jump it already fell back to `switch (pc)`. In the other 12 it emitted the loop as a forward `if`: the end label fell before the `if`, the brace-closing loop underflowed the unsigned `indentation`, and `indent()` appended about four billion tabs until `std::bad_alloc` (exit `0xC0000409`). A jump whose target is at or before itself now uses `switch (pc)`. So does a forward jump past an enclosing `if`'s end (7 shaders), which nested `if`s cannot express. 41 shaders take that path in total.
* **`ps` after scalar `SETP*`** (item 8 above).
* **Bounded `switch (pc)`.** The pc switch runs inside `for (pcGuard < 65536)` instead of `while (true)`. A backward jump repeats until a predicate fed by game constants flips, so a NaN or stale constant would spin until the host GPU removes the device. No such hang has been confirmed yet: the device-hung error that prompted the guard turned out to be a page fault (see `debugging.md`).
