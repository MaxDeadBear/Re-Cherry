# Where the renderer's facts come from

Every hardware or D3D fact the renderer relies on is taken from the game itself: the XDK types and D3D library code in the IDB (`assets/default.xex.i64`), UE3's own calls into it, or data the game writes at run time. Kameo and re:Blue are reference implementations where they agree with the game. Xenia, including the SDK's `rex/graphics` module and `rex/graphics/xenos.h` (Xenia's own source), is never used as a source, a reference or a justification.

## Device state

| fact | source |
| --- | --- |
| Device packet offsets (`m_WindowPacket` +0x28C0, `m_ValuesPacket` +0x28CC, `m_ControlPacket` +0x2934, ring pointers +0x30/+0x34/+0x38) | `D3DDevice` and `GPU_*PACKET` types in the IDB |
| `GPU_DEPTHCONTROL`, `GPU_BLENDCONTROL`, `GPU_COLORCONTROL`, `GPU_MODECONTROL`, `GPU_STENCILREFMASK`, `GPU_COLORMASK`, `GPU_POINT`, `GPU_VTECONTROL` bit layouts | the same types' bitfields |
| Cull mode values: 2 culls clockwise faces, 6 counter-clockwise | UE3's rasterizer state, `sub_82983560` |
| Blend factor values are D3DBLEND (6 source alpha, 7 inverse source alpha) | `D3DDevice_SetRenderState_SrcBlend` writes the value into `ColorSrcBlend`; UE3 calls it with 6 and 7 (`sub_8297A070`) |
| Compare function values (6 greater-or-equal, depth cleared to 0) | consistent with UE3's reversed-Z scene draws; no translation table found yet |
| `GPU_MISCPACKET.VtxControl` at +0x29C0; device init stores 4 (`PixCenter` = `GPUPIXCENTER_ZERO`, pixel centres on integer coordinates), so the host viewport moves +0.5 px | `GPU_VTXCONTROL` and `GPUPIXCENTER` in the IDB; `sub_82A00E20`; only `sub_829FCCC8` sets HALF, briefly |
| Depth bias: `SetRenderState_DepthBias` stores the D3D float unchanged in `PolyOffsetFront/BackOffset` (+0x2A54/+0x2A5C), `SlopeScaleDepthBias` stores it x16 in the scales (+0x2A50/+0x2A58), and `ModeControl` bits 11/12 enable them. UE3's rasterizer state (`sub_82983560`) negates both under its reversed depth (`dword_8335438C`), so the offset is in window depth | the two setters (0x829E1C18, 0x829E1B50) and their caller in the IDB |
| Back-face stencil ref/mask at +0x28FC | UE3's depth-stencil setter `sub_8297CA88` writes it (not used by the renderer yet) |

## Vertices

| fact | source |
| --- | --- |
| UE3 binds declarations through `D3DVertexShader_Bind(vs, decl, strides, ps)`, not the device | its only caller, UE3 `0x82982EA8` |
| Declaration type: format bits 0..5, signed bit 8, integer bit 9, selectors bits 10..21 (4 zero, 5 one), selectors XOR 1 when bits 6..7 = 01 (16-bit fields) | Bind's patcher `sub_829EC9C0` |
| Declaration count +24, max stream +28, id +48, elements +52 (12 bytes) | Bind and `sub_829EC9C0` |
| `GPUVERTEXFORMAT` names list fields from the high bits down: 10_11_11 (16) is X:11, Y:11, Z:10 from bit 0, 11_11_10 (17) the reverse | the IDB's `D3DDECLTYPE` enum (`HEND3N` 0x2A2190 uses 16, `DHEN3N` 0x2A2191 uses 17) and `2_10_10_10` being `DEC4`; UE3's skinned meshes use `HEND3N` positions |
| Stream n uses vertex fetch constant 95 - n | `sub_829EC9C0` |
| Vertex fetch constant: base address, endian (8-in-32 for D3D's own), size | `GPUVERTEX_FETCH_CONSTANT` type; `D3DDevice_BeginVertices` writes `size \| 0x10000002` |
| `BeginIndexedVertices` adds `BaseVertexIndex` to every index (it writes register 0x2102, `VGT_INDX_OFFSET`); the index endian is the low two bits of the `D3DFORMAT` (`INDEX16` 1, `INDEX32` 6, `LE_INDEX16` 0, `LE_INDEX32` 4) | its draw packet in the IDB; the `_D3DFORMAT` enum |
| A vertex shader starts with the vertex index in r0.x, and `vfetch` reads the vertex its index register names | UE3's sprite shader `140BDD1428AF0A5A` computes `r0.y = r0.x * c253.x + c252.x` before writing r0 and fetches with `r0.y`; its draws (`sub_82782340`) write one 56-byte vertex per particle for four vertices each |
| Rect list fourth corner v1 + v2 - v0 | UE3's unit square (0,0), (1,0), (0,1) in `sub_8297EBB0` |
| Index buffer width and endian bits | `XGSetIndexBufferHeader`, see guest-d3d.md |

## Textures and surfaces

| fact | source |
| --- | --- |
| Texture fetch constant fields (format, endian, base address, tiled, pitch, swizzle, filters, dimension, packed mips, Stacked flag) | `GPUTEXTURE_FETCH_CONSTANT` type |
| Sizes: 1D 24 bits, 3D 11:11:10, otherwise 13:13, plus one (three with a border); Stacked depth bits 26..31 | `sub_829E4470` (shared by the `GetLevelDesc` functions) and the type's `Size.Stack` union |
| Dimension 2 is a volume | `sub_829E4700` uses depth only when it is 2 |
| `SignX`..`SignW` (dword 0 bits 2..9) are `GPUSIGN` (0 unsigned, 1 signed, 2 bias, 3 gamma); gameplay fetches DXT1, DXT4_5 and 8888 textures with RGB gamma and alpha linear (signs 0x3F) into linear render targets, so the host decodes them, with the sRGB curve standing in for the console's piecewise-linear one | `GPUTEXTURE_FETCH_CONSTANT` and `GPUSIGN` in the IDB; the frame trace's `signs` |
| A 640x360 4x MSAA surface (`SurfaceInfo` 0x0A020280: pitch 640, `MsaaSamples` 2) at the scene's EDRAM base is resolved without being drawn: it averages 2x2 blocks of the 1280x720 scene | `GPU_SURFACEINFO` in the IDB; `--cherry_debug_edram_alias` |
| `2_10_10_10_FLOAT` (7e3) is unsigned with a largest value of 31.875; shaders write negative and huge values to it | the format; RenderDoc showed -1799..4584 in the host's 16-bit float stand-in |
| Colour and depth surface info (base, format) | `GPU_COLORINFO`, `GPU_DEPTHINFO` types |
| Resolve exponent bias: Resolve flags bits 26..31 become `GPU_COPYDESTINFO.CopyDestExpBias`; a surface's own `GPU_COLORINFO.ColorExpBias` (bits 20..25) | `D3DDevice_Resolve` (`srwi r9, r22, 26` at 0x829F2E20, packed into CopyDestInfo); the IDB's `GPU_COPYDESTINFO` and `GPU_COLORINFO` types |
| Native XG layout functions match the game's own | `--cherry_debug_verify_d3d` re-runs the originals: 70,000 calls through boot, menus and gameplay, no difference |
| Tiling, endian swaps and format tables | Kameo's `texture_codec` |

## The ring

| fact | source |
| --- | --- |
| Ring layout and readback words written by `SetRingBufferParameters` | its stores at `0x829EA3BC`..`0x829EA448` |
| Ring data comes from a custom allocator's fixed buffer (+152/+156) when one is installed at +0x3660 | `sub_829E8B70` |
| KickOff's return value is only used behind the ring-limit test; UE3's flushes ignore it | every one of its 131 call sites, checked in IDA |

## Shaders

XenosRecomp (hedge-dev) with `tools/xenosrecomp/cherry.patch`, whose generic fixes come from Kameo's `kameo.patch`; see [shaders.md](shaders.md).
