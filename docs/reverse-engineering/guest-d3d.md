# Lollipop Chainsaw (default.xex): statically linked XDK Direct3D

Every address and offset below was read out of the vanilla `default.xex` (base 0x82000000, `.text` 0x82200000-0x82EA4BBC) in IDA. The confirmed names are in `config/d3d.toml`.

---

## (a) Library bounds and XDK version

| range | contents | evidence |
| --- | --- | --- |
| `0x829DF440` - `0x82A00F30` | **D3D9 (d3d9ltcg.lib)** core: resources, state, draw, ring buffer, swap, device, GPU-hang dumper | first function `D3DResource_Destroy` (0x829DF440) uses `VdGlobalDevice`. Every `Vd*` import caller (VdSwap 0x829EBA3C, VdInitializeRingBuffer 0x829EA380, VdInitializeEngines 0x829FC27C, VdRetrainEDRAM 0x829FFEE4, ...) is inside this range. 0x829DF368 just below it is called from game code 0x829B8AE0 and is not D3D. |
| `0x82A01030` - `0x82A045AF` | **XGraphics (xgraphics.lib)**: tile/untile, XGSet*Header, XGOffsetResourceAddress, XGGetTextureDesc | called from UE3 and from D3DX at 0x82BA2520.. The NetDll thunks start at 0x82A045B0. |
| `0x82BA2520` - `0x82BAC4xx` | D3DX texture loader (not the D3D core) | calls CreateTexture, GetSurfaceLevel, LockRect, and similar |
| `0x82979F80` - `0x82988xxx` | **UE3 XeD3DDrv (the RHI)** | the strings `C:\CI\ASH\Development\Src\Xenon\XeD3DDrv\Src\XeD3DCommands.cpp` and `XeD3DVertexDeclaration.cpp` are referenced from here |

The range holds 320 functions. 106 of them have callers outside it.

**XDK version: 2.0.20871 (D3D 20871, LTCG build).** The GPU-hang dumper `sub_82A00468` prints `"D3D version %i.%i %s, kernel %i, frame %i"` with `li r5,0x5187` (20871), `r6 = 0` and `"LTCG"`. It also prints `"Device 0x%x, Snooped 0x%x, NonSnooped"` using device+0x2B10 and device+0x2B14.

### This is the LTCG library, and that drives the hook design

The title links **d3d9ltcg.lib**. With link-time code generation the linker has:

1. **Inlined most of the small D3D API into UE3.** None of these exist as functions:
   - SetRenderState (except about 10 setters)
   - SetSamplerState (except about 6)
   - SetVertexDeclaration
   - EndVertices and EndIndexedVertices
   - InsertFence
   - Clear (the D3DCOLOR wrapper)
   - Get* accessors
   - D3DVertexBuffer_Lock/Unlock and D3DIndexBuffer_Lock/Unlock
   - CreateVertexBuffer/CreateIndexBuffer, CreatePixelShader/CreateVertexShader, CreateVertexDeclaration
   - Present and Reset (not in the binary at all)

   UE3 writes the device shadow directly, for example `*(dev+0x2FD0) = decl; *(u64*)(dev+0x10) |= 0x80000` (SetVertexDeclaration at 0x8297BC18, 0x8297BC50, 0x8297A1A0, 0x8297EF60, 0x82986A60).
2. **Specialised the surviving functions by constant propagation and dead-argument elimination.** Several signatures are *not* the XDK prototypes:
   - `ClearF` has lost `Stencil`.
   - `Resolve` has lost `DestLevel` and `pParameters`.
   - `DrawIndexedVertices` has lost `BaseVertexIndex`.
   - `BeginTiling` has lost `pClearColor` and `ClearStencil`.
   - `EndTiling` has lost every argument except the device.
   - `Direct3D_CreateDevice` takes only `pPresentationParameters` and stores the device straight into UE3's global `GDirect3DDevice` at 0x8332FDDC.
   - `D3DQuery_GetData` has lost `Size` and `Flags`.

   The register-level signatures in (c) are the ones to marshal. **Do not copy XDK or reblue prototypes blindly.**

Consequence: a native renderer cannot intercept state by hooking setters. It must read the device shadow at draw time, as the Kameo renderer does. The draw, resolve, swap, resource and fence entry points below do exist and are hookable.

---

## (b) TOML `[functions]` block

### Confirmed (renamed in IDA)

```toml
# ---- Direct3D (XDK 2.0.20871, d3d9ltcg) — confirmed ----
0x829DF440 = { name = "D3DResource_Destroy", size = 0x2F4 }  # confirmed: called by Release at refcount 0; jump table on Common&0xF type, frees through the physical-memory allocator sub_82BF5368
0x829DFC70 = { name = "D3DResource_AddRef", size = 0x74 }  # confirmed: lwarx/stwcx ++(res+4); recurses to parent (Common&0x40000000 → res+0x18)
0x829DFCE8 = { name = "D3DResource_Release", size = 0x80 }  # confirmed: atomic --(res+4); 0 → D3DResource_Destroy; 92 external callers
0x829DFD68 = { name = "D3DResource_GetDevice", size = 0x78 }  # confirmed: reads VdGlobalDevice, ++device+0x3C, *out = device
0x829DFDE0 = { name = "D3DResource_GetType", size = 0x8C }  # confirmed: Common&0xF plus Volume16/VolumeTex17/Cube18/Array19/Line20 from fetch dim bits
0x829DFE70 = { name = "D3DResource_IsSet", size = 0xEC }  # confirmed: compares the resource against device stream/IB/RT/DS/texture/shader slots
0x829E10C0 = { name = "D3DDevice_SetRenderState_AlphaBlendEnable", size = 0x80 }  # confirmed: dev+0x2FFC bit31; FALSE writes 0x10001 (ONE/ZERO/ADD) to BlendControl0-3
0x829E11E0 = { name = "D3DDevice_SetRenderState_SrcBlend", size = 0x7C }  # confirmed: dev+0x2FF8 bits0-4; UE3 splash passes 6 (SRCALPHA)
0x829E1270 = { name = "D3DDevice_SetRenderState_DestBlend", size = 0x7C }  # confirmed: dev+0x2FF8 bits8-12; UE3 splash passes 7 (INVSRCALPHA)
0x829E1450 = { name = "D3DDevice_SetRenderState_SeparateAlphaBlendEnable", size = 0x84 }  # confirmed: dev+0x2FFC bit30, rebuilds BlendControl0-3
0x829E1B38 = { name = "D3DDevice_SetRenderState_ScissorTestEnable", size = 0xC }  # confirmed: dev+0x3000 = v; tail-calls SetScissorRect(dev, dev+0x32FC)
0x829E1B50 = { name = "D3DDevice_SetRenderState_SlopeScaleDepthBias", size = 0xA8 }  # confirmed: value*16 → dev+0x2A50/0x2A58, poly-offset enables in ModeControl (dev+0x2948)
0x829E1C18 = { name = "D3DDevice_SetRenderState_DepthBias", size = 0x9C }  # confirmed: → dev+0x2A54/0x2A5C, same enable bits
0x829E28F8 = { name = "D3DDevice_SetSamplerState_MinFilter", size = 0xC8 }  # confirmed: fetch dword3 bits21-22 + aniso bits25-27 at dev+0x480+24*s
0x829E2AA0 = { name = "D3DDevice_SetSamplerState_MagFilter", size = 0xC8 }  # confirmed: fetch dword3 bits19-20 + aniso
0x829E2D40 = { name = "D3DDevice_SetSamplerState_MaxAnisotropy", size = 0x68 }  # confirmed: byte dev+0x2A70+s, aniso bits25-27 via table 0x821A5598
0x829E2E60 = { name = "D3DDevice_SetSamplerState_MipMapLodBias", size = 0x60 }  # confirmed: (float*32) → fetch dword4 bits12-21
0x829E2F08 = { name = "D3DDevice_SetSamplerState_MaxMipLevel", size = 0x6C }  # confirmed: byte dev+0x3044+s, fetch dword4 bits2-5 clamped to texture
0x829E2F88 = { name = "D3DDevice_SetSamplerState_MinMipLevel", size = 0x6C }  # confirmed: byte dev+0x305E+s, fetch dword4 bits6-9
0x829E3328 = { name = "D3DDevice_SetScissorRect", size = 0xF8 }  # confirmed: RECT → dev+0x32FC..0x3308, WindowScissorTL/BR shadow dev+0x28C4/0x28C8
0x829E3420 = { name = "D3DDevice_SetViewport", size = 0x7C }  # confirmed: D3DVIEWPORT9* → int→float → sub_829E0DC8 (VPORT scale/offset regs dev+0x2908..0x291C)
0x829E34A0 = { name = "D3DDevice_SetStreamSource", size = 0x120 }  # confirmed: VB → dev+0x3224+4n, stride>>2 → byte dev+0x3268+n, vertex fetch const dev+0x778-8n
0x829E35C0 = { name = "D3DDevice_SetIndices", size = 0x90 }  # confirmed: dev+0x320C = IB (fence-tags the old one)
0x829E3650 = { name = "D3DDevice_SetRenderTarget", size = 0x390 }  # confirmed: dev+0x3210+4*idx; ColornInfo from surf+0x1C
0x829E39E0 = { name = "D3DDevice_SetDepthStencilSurface", size = 0x2D8 }  # confirmed: dev+0x3220; DepthInfo dev+0x2888 = surf+0x1C
0x829E3CB8 = { name = "D3DDevice_SetGammaRamp", size = 0xCC }  # confirmed: D3DGAMMARAMP → 1536-byte PWL at dev+0x3C30 (sub_829F4FD8), sets the dev+0x42DC|=2 dirty bit Swap consumes
0x829E4AD0 = { name = "D3DTexture_GetLevelDesc", size = 0x100 }  # confirmed: fills D3DSURFACE_DESC {Format,Type,0,0,0,0,W,H}
0x829E4BD0 = { name = "D3DVolumeTexture_GetLevelDesc", size = 0x100 }  # confirmed: D3DVOLUME_DESC variant (W,H,D)
0x829E5988 = { name = "D3DTexture_GetSurfaceLevel", size = 0xD0 }  # confirmed: allocs a 48-byte child surface (type 4|0x40000000), parent at +0x18, level<<28 at +0x1C
0x829E5A58 = { name = "D3DCubeTexture_GetCubeMapSurface", size = 0xB4 }  # confirmed: same with face<<22
0x829E5B18 = { name = "D3DVolumeTexture_GetVolumeLevel", size = 0xC4 }  # confirmed: 32-byte child volume
0x829E5BE8 = { name = "D3DDevice_CreateTexture", size = 0x120 }  # confirmed: (W,H,Depth,Levels,Usage,Format,Pool,Type) → 52-byte texture + physical alloc; no device arg (as reblue)
0x829E5D08 = { name = "D3DDevice_CreateSurface", size = 0x164 }  # confirmed: (W,H,Format,MultiSample,pParameters) → 48-byte surface + EDRAM tiles (sub_829F4A30, 0x800-tile limit)
0x829E5E70 = { name = "D3DSurface_GetDesc", size = 0x98 }  # confirmed: from surf+0x24 size bits / parent GetLevelDesc
0x829E5F08 = { name = "D3DSurface_LockRect", size = 0x20 }  # confirmed: forwards to parent LockRect(face=(+0x1C>>22)&0x3F, level=+0x1C>>28)
0x829E5F28 = { name = "D3DVolume_GetDesc", size = 0x44 }  # confirmed: parent VolumeTexture_GetLevelDesc, Type=16
0x829E5F70 = { name = "D3DVolume_LockBox", size = 0x1C }  # confirmed: parent LockBox
0x829E5FA8 = { name = "D3DDevice_SetTexture", size = 0x18C }  # confirmed: dev+0x3278+4s = tex; builds fetch const at dev+0x480+24s from tex+0x1C..0x33
0x829E65B8 = { name = "D3DDevice_SetVertexShaderConstantFN", size = 0xD8 }  # confirmed: copies float4s to dev+0x780+16*reg, ORs mask into u64 dev+0x00
0x829E6690 = { name = "D3DDevice_SetPixelShaderConstantFN", size = 0xD8 }  # confirmed: dev+0x1780+16*reg, mask into u64 dev+0x08
0x829E67A0 = { name = "D3DDevice_SetPixelShader", size = 0x1BC }  # confirmed: dev+0x330C; replays the shader's {u16 off,u16 count} register table into dev+0x480
0x829E6960 = { name = "XGRegisterVertexShader", size = 0xF8 }  # confirmed: memset 872 bytes, type 6, same size as reblue's XGRegisterVertexShader
0x829E6A58 = { name = "D3DDevice_SetVertexShader", size = 0x1CC }  # confirmed: dev+0x3310; same table replay (shader+0x37C)
0x829E6D18 = { name = "D3DDevice_SetShaderGPRAllocation", size = 0xE8 }  # confirmed: (dev,Flags,VS,PS) defaults 64/64, writes the GPR allocation of GPU_PROGRAMCONTROL
0x829E6E00 = { name = "D3DDevice_BeginVertices", size = 0x4BC }  # confirmed: DRAW_INDX (0x22) immediate; vertex data from secondary ring; resume ptr → dev+0x3614
0x829E72C0 = { name = "D3DDevice_DrawVerticesUP", size = 0x48 }  # confirmed: BeginVertices + memcpy(count*stride) + inline EndVertices (dev+0x30 = dev+0x3614)
0x829E7308 = { name = "D3DDevice_BeginIndexedVertices", size = 0x540 }  # confirmed: matches the XDK 9-arg prototype; returns E_OUTOFMEMORY 0x8007000E on failure
0x829E7848 = { name = "D3DDevice_DrawVertices", size = 0x414 }  # confirmed: (dev,Prim,StartVertex,VertexCount); IndxOffset=StartVertex, DRAW_INDX auto-index, splits >0xFFFF
0x829E7C60 = { name = "D3DDevice_DrawIndexedVertices", size = 0x474 }  # confirmed: LTCG clone (dev,Prim,StartIndex,IndexCount); reads IB dev+0x320C; BaseVertexIndex folded to 0
0x829E9B68 = { name = "D3D_CDevice_KickOff", size = 0x100 }  # confirmed: the "write ptr > soft end" handler at 121 internal sites; submits segment, returns new dev+0x30
0x829E9C68 = { name = "D3D_CDevice_BlockOnFence", size = 0xF4 }  # confirmed: (dev,fence,reason,flag); spins on *(dev+0x2B10) vs dev+0x2B1C
0x829EA078 = { name = "D3DDevice_BlockUntilIdle", size = 0x7C }  # confirmed: WAIT_UNTIL(0x5C8)=0x20000, BlockOnFence(dev+0x2B1C,4), spins on dev+0x2B90
0x829EA0F8 = { name = "D3DDevice_BlockOnFence", size = 0x64 }  # confirmed: (Fence) only, gets the device from VdGlobalDevice, reason 6
0x829EA160 = { name = "D3DDevice_IsFencePending", size = 0x90 }  # confirmed: (Fence) → KickOff if fence==current, return (cur-f) < (cur-*readback)
0x829EA1F0 = { name = "D3DDevice_SetRingBufferParameters", size = 0x3F8 }  # confirmed: VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack(readback+0x3C)
0x829EA918 = { name = "D3DDevice_SynchronizeToPresentationInterval", size = 0x1E0 }  # confirmed: present interval from dev+0x2FFC bits23-29, vblank-callback packet (sub_829EA768 flips D1GRPH 0x7FC86110); midasm hook 0x829EAA14 is inside it
0x829EB660 = { name = "D3DDevice_Swap", size = 0x834 }  # confirmed: (dev, pFrontBuffer, pScalerParams); VdGetSystemCommandBuffer + VdSwap at 0x829EBA3C (midasm hook), ++frame dev+0x4248
0x829EE800 = { name = "Direct3D_CreateDevice", size = 0xE8 }  # confirmed: LTCG clone (pPresentParams); allocs 0x6100, aligns 128 (sizeof 0x6080), → sub_829FC210, stores to 0x8332FDDC
0x829EE8E8 = { name = "D3DDevice_Release", size = 0x84 }  # confirmed: --dev+0x3C; 0 → sub_829FC4F8 (VdShutdownEngines) + free
0x829EEB58 = { name = "D3DDevice_GetDeviceCaps", size = 0x14 }  # confirmed: memcpy(pCaps, 0x821A5868, 304)
0x829EED38 = { name = "D3DQuery_Release", size = 0x260 }  # confirmed: --q+0xC; frees occlusion (9) / screen-extent (10) slots
0x829EEF98 = { name = "D3DQuery_Issue", size = 0x2B0 }  # confirmed: flags 2=BEGIN 1=END; types 8 event/9 occlusion/10 screen extent; stamps q+0x18 = dev+0x2B1C
0x829EF248 = { name = "D3DQuery_GetData", size = 0x360 }  # confirmed: LTCG clone (pQuery, pData); byte-swaps the ZPASS counters from memory; S_FALSE while the fence is pending
0x829F2468 = { name = "D3DDevice_ClearF", size = 0x134 }  # confirmed: LTCG clone (dev,Flags,pRect,pColor,Z) → sub_829F1DF0 rect-list clear; 22 inlined-Clear callers
0x829F2BA8 = { name = "D3DDevice_Resolve", size = 0x1048 }  # confirmed: LTCG clone; UE3 present calls (dev,0,0,FrontBufferTex,0,0,0,0.0)
0x829F3D10 = { name = "D3DDevice_SetPredication", size = 0x170 }  # confirmed: dev+0x331C mask, type-3 SET_CONSTANT predicate packet 0xC0002100
0x829F3E80 = { name = "D3DDevice_BeginTiling", size = 0x354 }  # confirmed: LTCG clone (dev,Flags,Count,pTileRects,ClearZ); tile rects → dev+0x3348.., ClearF(63)
0x829F41D8 = { name = "D3DDevice_EndTiling", size = 0x344 }  # confirmed: LTCG clone (dev) only; predicated replay of the tiling command buffer
0x82A01BB8 = { name = "XGUntileSurface", size = 0x4 }  # confirmed: thunk → 0x82A016D0; used by RHIReadSurfaceData (dst,pitch,pPoint,src,w,h,pRect,texelPitch)
0x82A01BC0 = { name = "XGSetVertexBufferHeader", size = 0x94 }  # confirmed: (Length,Usage,Pool,BaseOffset,pVB): Common=1, +0x18=addr|3, +0x1C=len|0x10000002
0x82A01C58 = { name = "XGSetIndexBufferHeader", size = 0x90 }  # confirmed: (Length,Usage,Format,Pool,BaseOffset,pIB): Common=2|fmt<<29
0x82A02048 = { name = "XGOffsetResourceAddress", size = 0xD4 }  # confirmed: per-type base-address patch (VB +0x18, tex via 0x82A01FE0, shader +0x20)
```

### Likely (not renamed in IDA)

```toml
# ---- likely ----
# 0x829DF738 = { name = "D3D_LockResource", size = 0x31C }            # likely: (res, BlockReason[12 VB,10 IB,14 tex], ..., Flags r10); blocks on fence, returns CPU ptr; core of the inlined VB/IB/Texture Lock
# 0x829DFA58 = { name = "D3DResource_Unlock", size = 0x158 }          # likely: (res, baseAddr, mipAddr); flushes dirty CPU range (sub_829F4C30), --lock count (res-=0x100)
# 0x829E0440 = { name = "D3DDevice_BeginExport", size = 0x11C }       # likely: memexport stream address → dev+0x2898, nesting counter dev+0x3650
# 0x829E0560 = { name = "D3DDevice_EndExport", size = 0x230 }         # likely: flushes/invalidates the exported range, --dev+0x3650
# 0x829E0790 = { name = "D3DDevice_InvalidateGpuCache", size = 0x198 } # likely: (baseAddr, size) → type-0 0xA31 coherency packets
# 0x829E0DC8 = { name = "D3DDevice_SetViewportF", size = 0x25C }      # likely: float viewport core (X,Y,W,H,MinZ,MaxZ in f1-f6)
# 0x829E22D8 = { name = "D3DDevice_SetRenderState_HighPrecisionBlendEnable", size = 0x90 }   # likely: flips RT0 color format between 2_10_10_10 and its float variants
# 0x829E2370 = { name = "D3DDevice_SetRenderState_HighPrecisionBlendEnable1", size = 0x90 }  # likely: same for RT1 (dev+0x288C)
# 0x829E3D88 = { name = "D3DDevice_SetPWLGamma", size = 0xCC }        # likely: twin of SetGammaRamp using the PWL converter sub_829F5070; no callers
# 0x829E3E58 = { name = "D3DDevice_SetBlendState", size = 0x58 }      # likely: (dev, RTIndex, packed) → BlendControl0 dev+0x2938 / RT1-3 dev+0x2958..0x2960
# 0x829E5210 = { name = "D3DCubeTexture_LockRect", size = 0x8C }      # likely: generic (tex, face/arrayIndex, level, pLockedRect, pRect, Flags); used for 2D textures with face 0
# 0x829E52A0 = { name = "D3DVolumeTexture_LockBox", size = 0xA8 }     # likely
# 0x829E58B0 = { name = "D3DBaseTexture_GetLevelCount", size = 0x10 } # likely: ((tex+0x2C>>6)&0xF)+1
# 0x829E58C0 = { name = "D3DTexture_LockTail", size = 0xC8 }          # likely: locks from XGGetMipTailBaseLevel
# 0x829E5F90 = { name = "D3DSurface_UnlockRect", size = 0x18 }        # likely: Unlock on the parent (shared by surfaces and volumes)
# 0x829E6768 = { name = "XGRegisterPixelShader", size = 0x38 }        # likely: type 7 header init
# 0x829E6C28 = { name = "XGSetVertexDeclarationHeader", size = 0xEC } # likely: builds a 56+12n-byte decl (type 5) from a D3DVERTEXELEMENT9 array into caller memory = CreateVertexDeclaration core
# 0x829E80D8 = { name = "D3DDevice_DrawTessellatedVertices", size = 0x4A0 } # likely: same as DrawVertices plus tessellation regs, VGT major mode 0x100
# 0x829E8578 = { name = "D3D_CDevice_ReserveRing", size = 0x78 }      # likely: ensure N dwords before the hard end (dev+0x34), else KickOff/StartNewSegment
# 0x829E8B70 = { name = "D3D_CDevice_AllocateRingData", size = 0x1A0 } # likely: (dev, dwords, align) carve from the secondary ring (vertex data, Swap packet)
# 0x829E8940 = { name = "D3D_RingBufferDeviceAllocate", size = 0x230 } # likely: the default allocator (flag 1 data / 2 segment); custom allocators come through dev+0x3660 cb+0xAC
# 0x829E8D10 = { name = "D3D_CDevice_StartNewSegment", size = 0x1DC } # likely: sets dev+0x30/+0x34/+0x38 (soft = hard - 160)
# 0x829E8EF0 = { name = "D3D_CDevice_BlockOnPrimaryRange", size = 0xC8 } # likely: mask dev+0x3BC8, RPtr writeback readback+0x3C
# 0x829E8888 = { name = "D3D_CDevice_BlockOnSecondaryPosition", size = 0xB4 } # likely
# 0x829E8FB8 = { name = "D3D_CDevice_WritePrimary", size = 0x118 }    # likely: copies packets into the primary ring (base dev+0x3BC4, mask dev+0x3BC8, index dev+0x2B50)
# 0x829E95E8 = { name = "D3D_CDevice_InsertSegmentFence", size = 0x140 } # likely: EVENT_WRITE_SHD fence to readback[0], dev+0x2B1C += 2
# 0x829E9D60 = { name = "D3D_CDevice_InsertCallback", size = 0x280 } # likely: interrupt-callback packets (used by SynchronizeToPresentationInterval and EndTiling)
# 0x829EA768 = { name = "D3D_PresentIntervalInterruptCallback", size = 0x1AC } # likely: vblank ISR; writes the front-buffer address to D1GRPH_PRIMARY_SURFACE_ADDRESS 0x7FC86110
# 0x829EE1B0 = { name = "D3DVertexShader_Bind", size = 0x184 }       # likely: (vs, decl, strides[], ps) patches fetches to match the decl (called from UE3 0x82982BB8)
# 0x829EE970 = { name = "D3DDevice_FlushHiZStencil", size = 0x64 }   # likely: EVENT_WRITE 15, clears the dev+0x2B3D bit 2 that Clear sets
# 0x829EE9D8 = { name = "D3DDevice_UnsetAll", size = 0x180 }         # likely: unbinds RT/DS/VS/PS/IB/16 streams/26 textures
# 0x829EEC98 = { name = "D3DDevice_CreateQuery", size = 0x9C }       # likely: LTCG clone (dev) → 156-byte occlusion query (type 9)
# 0x829F4798 = { name = "D3DDevice_QueryBufferSpace", size = 0x154 } # likely: (dev, pUsed, pRemaining)
# 0x829F4C30 = { name = "D3D_FlushCachedMemory", size = 0xA8 }       # likely: dcbf loop over [a,b) unless the address is write-combined
# 0x829FBF68 = { name = "D3D_SetDisplayMode", size = 0x1B4 }         # likely: VdSetDisplayMode/Override; only Swap (flag 8) calls it
# 0x829FC210 = { name = "D3D_CDevice_Initialize", size = 0x2E8 }     # likely: VdInitializeEngines, VdSetGraphicsInterruptCallback, ring setup
# 0x829FC4F8 = { name = "D3D_CDevice_Destroy", size = 0x3A4 }        # likely: VdShutdownEngines
# 0x82A00468 = { name = "D3D_DumpGpuHang", size = 0x8B4 }            # likely
# 0x82A01BB0 = { name = "XGTileSurface", size = 0x4 }                # likely: thunk → 0x82A01300, twin of XGUntileSurface
# 0x82A01EF0 = { name = "XGSetTextureHeader", size = 0x74 }          # likely: → 0x82A01CE8 with type 1
# 0x82A01F68 = { name = "XGSetCubeTextureHeader", size = 0x78 }      # likely: → 0x82A01CE8 with W==H, type 6
# 0x82A02870 = { name = "XGGetTextureDesc", size = 0x4 }             # likely: thunk → 0x82A02440 (tiled flag 0x100, W, H, pitch, bpp)
# 0x82A02B80 = { name = "XGGetMipTailBaseLevel", size = 0x5C }       # likely
```

---

## (c) Guest signatures (registers) for the confirmed set

### Xenon ABI reminder

Integer arguments go in r3..r10. A `float`/`double` argument goes in the next FPR (f1, f2, ...) **and still consumes the GPR slot at its position**, so the next integer argument skips one register. Example: Resolve's `ClearZ` is in f1 and occupies r10, so `ClearStencil` goes to the stack slot at caller `sp+0x54` (the 9th slot). reblue's `D3DDevice_Clear_hook` uses a placeholder parameter to absorb the skipped GPR for the same reason. Return values are in r3 (or f1).

`dev` = `D3DDevice*`. In UE3 it always comes from the global `GDirect3DDevice` at **0x8332FDDC**. The D3D library itself also gets it from the kernel export `VdGlobalDevice` (`VdGlobalXamDevice` in the XAM process).

#### Device

| function | args | ret |
| --- | --- | --- |
| `Direct3D_CreateDevice` | r3 = `D3DPRESENT_PARAMETERS*` (Adapter, DeviceType, hFocus, BehaviorFlags and ppDevice are gone; the device is written to *0x8332FDDC) | r3 HRESULT (0 or 0x8007000E) |
| `D3DDevice_Release` | r3 dev | r3 new refcount |
| `D3DDevice_GetDeviceCaps` | r3 dev, r4 `D3DCAPS9*` (304 bytes) | |
| `D3DDevice_SetRingBufferParameters` | r3 dev, r4 `D3DRING_BUFFER_PARAMETERS*` (+4 PrimarySize, +8 pPrimary, +0xC SecondarySize, +0x10 pSecondary, +0x14 SegmentCount) | r3 HRESULT |

#### Swap and present

| function | args | ret |
| --- | --- | --- |
| `D3DDevice_Swap` | r3 dev, r4 `D3DBaseTexture* pFrontBuffer` (52 bytes copied to dev+0x3B90), r5 `const D3DVIDEO_SCALER_PARAMETERS*` (28 bytes, compared to dev+0x36DC; may be NULL) | |
| `D3DDevice_SynchronizeToPresentationInterval` | r3 dev, r4 = 0 from UE3 (internally Swap passes the flip address) | |

#### Draw

| function | args | ret |
| --- | --- | --- |
| `D3DDevice_ClearF` | r3 dev, r4 Flags (bits 0-3 TARGET0..3, 0x10 ZBUFFER, 0x20 STENCIL), r5 `const D3DRECT*` (NULL = full surface), r6 `const D3DVECTOR4*` color (NULL = 0), f1 Z (GPR slot r7 skipped). No stencil argument. | |
| `D3DDevice_DrawVertices` | r3 dev, r4 PrimType, r5 StartVertex, r6 VertexCount | |
| `D3DDevice_DrawIndexedVertices` | r3 dev, r4 PrimType, r5 StartIndex, r6 IndexCount (BaseVertexIndex = 0; UE3 offsets streams instead) | |
| `D3DDevice_DrawVerticesUP` | r3 dev, r4 PrimType, r5 VertexCount, r6 `const void*` data, r7 stride | r3 (ptr or 0) |
| `D3DDevice_BeginVertices` | r3 dev, r4 PrimType, r5 VertexCount, r6 stride | r3 `void*` vertex dest (0 on failure) |
| `D3DDevice_BeginIndexedVertices` | r3 dev, r4 PrimType, r5 BaseVertexIndex, r6 NumVertices, r7 IndexCount, r8 IndexDataFormat (bit 2 = INDEX32), r9 stride, r10 `void** ppIndexData`, stack slot 8 `void** ppVertexData` | r3 HRESULT |
| `D3DDevice_Resolve` | r3 dev, r4 Flags (bits 0-2 source: 0-3 RT, 4 depth; 0x70 fragments; 0x100 clear color; 0x200 clear depth; 0x4000/0x8000 are gated by the globals 0x833549F4/0x833549F8; their meaning is unverified), r5 `const D3DRECT* pSourceRect`, r6 `D3DBaseTexture* pDestTexture`, r7 `const D3DPOINT* pDestPoint`, r8 DestSliceOrFace, r9 `const D3DVECTOR4* pClearColor`, f1 ClearZ (GPR slot r10 skipped), stack slot 8 (caller sp+0x54) ClearStencil. DestLevel and pParameters are gone (0/NULL). | |
| `D3DDevice_BeginTiling` | r3 dev, r4 Flags, r5 Count, r6 `const D3DRECT* pTileRects`, f1 ClearZ (r7 slot skipped) | |
| `D3DDevice_EndTiling` | r3 dev only | |
| `D3DDevice_SetPredication` | r3 dev, r4 PredicationMask | |

#### State

| function | args |
| --- | --- |
| `D3DDevice_SetRenderTarget` | r3 dev, r4 index 0-3, r5 `D3DSurface*` |
| `D3DDevice_SetDepthStencilSurface` | r3 dev, r4 `D3DSurface*` |
| `D3DDevice_SetViewport` | r3 dev, r4 `const D3DVIEWPORT9*` {X,Y,W,H u32; MinZ,MaxZ f32} |
| `D3DDevice_SetScissorRect` | r3 dev, r4 `const RECT*` |
| `D3DDevice_SetTexture` | r3 dev, r4 sampler 0-25, r5 `D3DBaseTexture*`, r6 u64 PendingMask (the fetch dirty bit, ORed into dev+0x18) |
| `D3DDevice_SetStreamSource` | r3 dev, r4 stream 0-15, r5 `D3DVertexBuffer*`, r6 OffsetInBytes, r7 Stride, r8 u64 PendingMask |
| `D3DDevice_SetIndices` | r3 dev, r4 `D3DIndexBuffer*` |
| `D3DDevice_SetVertexShader` / `D3DDevice_SetPixelShader` | r3 dev, r4 shader* |
| `D3DDevice_SetVertexShaderConstantFN` / `D3DDevice_SetPixelShaderConstantFN` | r3 dev, r4 StartRegister, r5 `const float*` (float4s, may be unaligned), r6 Vector4fCount, r7 u64 PendingMask |
| `D3DDevice_SetShaderGPRAllocation` | r3 dev, r4 Flags, r5 VS GPRs, r6 PS GPRs |
| `D3DDevice_SetGammaRamp` | r3 dev, r4 `const D3DGAMMARAMP*` (Flags gone) |
| `D3DDevice_SetRenderState_*` | r3 dev, r4 value; the bias setters take the value as a float bit pattern in r4 (`*(float*)&a2`) |
| `D3DDevice_SetSamplerState_*` | r3 dev, r4 sampler, r5 value (MipMapLodBias: float bits) |

#### Fences

| function | args | ret |
| --- | --- | --- |
| `D3D_CDevice_KickOff` | r3 dev | r3 new write pointer |
| `D3D_CDevice_BlockOnFence` | r3 dev, r4 fence, r5 reason, r6 flag | |
| `D3DDevice_BlockOnFence` | r3 Fence | |
| `D3DDevice_IsFencePending` | r3 Fence | r3 BOOL |
| `D3DDevice_BlockUntilIdle` | r3 dev | |

#### Resources and queries

| function | args | ret |
| --- | --- | --- |
| `D3DDevice_CreateTexture` | r3 W, r4 H, r5 Depth, r6 Levels, r7 Usage, r8 Format (GPUTEXTUREFORMAT-style D3DFORMAT, e.g. 0x28280106), r9 Pool, r10 D3DRESOURCETYPE (3 tex, 0x11 volume, 0x12 cube, 0x13 array) | r3 `D3DBaseTexture*` |
| `D3DDevice_CreateSurface` | r3 W, r4 H, r5 Format, r6 MultiSample, r7 `D3DSURFACE_PARAMETERS*` (NULL → auto EDRAM base) | r3 `D3DSurface*` |
| `D3DTexture_GetLevelDesc` | r3 tex, r4 level, r5 `D3DSURFACE_DESC*` | |
| `D3DTexture_GetSurfaceLevel` | r3 tex, r4 level | r3 `D3DSurface*` (AddRefs the parent) |
| `D3DCubeTexture_GetCubeMapSurface` | r3 cube, r4 face, r5 level | r3 surface |
| `D3DVolumeTexture_GetVolumeLevel` | r3 tex, r4 level | r3 volume |
| `D3DSurface_GetDesc` | r3 surf, r4 desc | |
| `D3DSurface_LockRect` | r3 surf, r4 `D3DLOCKED_RECT*`, r5 `const RECT*`, r6 Flags | |
| `D3DVolume_LockBox` | r3 vol, r4 `D3DLOCKED_BOX*`, r5 `const D3DBOX*`, r6 Flags | |
| `D3DResource_AddRef` / `Release` / `GetType` | r3 res | r3 |
| `D3DResource_GetDevice` | r3 res, r4 `D3DDevice**` | |
| `D3DResource_IsSet` | r3 res, r4 dev | r3 BOOL |
| `D3DResource_Destroy` | r3 res | |
| `D3DQuery_Issue` | r3 query, r4 Flags (2 BEGIN, 1 END) | |
| `D3DQuery_GetData` | r3 query, r4 pData (DWORD for occlusion, 24 bytes for screen extent) | r3 0 ok / 1 pending / 0x8876086A not issued |
| `D3DQuery_Release` | r3 query | |
| `XGRegisterVertexShader` | r3 shader memory (872 bytes + ucode), r4 pFunction | |
| `XGSetVertexBufferHeader` | r3 Length, r4 Usage, r5 Pool, r6 BaseOffset, r7 `D3DVertexBuffer*` | |
| `XGSetIndexBufferHeader` | r3 Length, r4 Usage, r5 Format, r6 Pool, r7 BaseOffset, r8 `D3DIndexBuffer*` | |
| `XGOffsetResourceAddress` | r3 res, r4 base address | |
| `XGUntileSurface` | r3 dst, r4 rowPitch, r5 `const POINT*`, r6 src, r7 W, r8 H, r9 `const RECT*`, r10 texelPitch | |

#### EndVertices is inlined everywhere

The inlined form is `lwz rX,0x3614(dev); stw rX,0x30(dev)`. It appears in:

- 0x8230A8C8
- 0x823AE154
- 0x8248B620
- 0x8277FC4C and 0x8277FFB8
- 0x8278248C and 0x82782534
- 0x827D97D0
- 0x82949A20, 0x8294E928, 0x82950174, 0x82950E10, 0x82956FF0, 0x82957A3C, 0x829580CC, 0x82958DA0
- 0x8297DD10, 0x8297E294, 0x8297F250
- 0x829E72C0 (inside DrawVerticesUP)

`sub_824FA6E8` also touches +0x3614 but on another struct; ignore it.

A BeginVertices hook therefore never sees an End call. Either flush the pending UP draw at the next D3D entry point or at Swap (the reblue approach assumes EndVertices exists), or hook the UE3 wrappers that bracket it (section (e)).

---

## (d) D3DDevice layout (verified)

`sizeof(D3DDevice) = 0x6080`: CreateDevice memsets 24704 bytes at a 128-aligned pointer and keeps the raw allocation at dev-4.

**Compared with Kameo, most of the struct is shifted:**

| Kameo region | offset in this build |
| --- | --- |
| dirty masks | Kameo - 0x10 |
| ring pointers | moved to +0x30 |
| register shadow 0x2CC0.. | Kameo - 0x440 (checked independently on scissor, DepthControl flush, poly offset and viewport) |
| bound-object slots | Kameo + 0x118 (RT, DS, IB) |
| fetch-constant block 0x480, VS consts 0x780, PS consts 0x1780, bool consts 0x2780 | unchanged |

| offset | field | evidence |
| --- | --- | --- |
| +0x000 u64 | VS float-constant dirty mask (chunk k = 4 regs) | SetVertexShaderConstantFN ORs into it; the draw flush calls `sub_829EC870(dev, mask, 0x4000, dev+0x780)` |
| +0x008 u64 | PS float-constant dirty mask | PS FN; flush 0x4400 from dev+0x1780 |
| +0x010 u64 | render-state dirty: low&0xFFF → 0x2200 block, &0x1F000 → 0x2180 block, &0xFFE00000 → 0x2100 block, 0x1E0000 → sub_829ED830; bit 0x80000 = vertex decl/stride changed, 0x100000 = shader | draw prologue in all draw functions |
| +0x018 u64 | fetch-constant dirty (textures and vertex fetch), flushed by sub_829EC718 | SetTexture/SetStreamSource OR PendingMask here |
| +0x020 u64 | misc dirty: bool consts (0x4900 from dev+0x2780) and the 0x2300 block from dev+0x29B8 | draw prologue |
| +0x028 u64 | shader-dependent mask (ANDed with +0x10) | |
| **+0x030** | **ring write pointer** (last written dword; packets go to `ptr+4`) | every `if (*(dev+0x30) > *(dev+0x38)) KickOff` site |
| +0x034 | segment hard end | StartNewSegment / ReserveRing |
| +0x038 | soft end (= hard - 160; crossing it forces KickOff) | |
| +0x03C | device refcount | Release / GetDevice |
| +0x480 | fetch constants, 32 × 24 bytes. Texture fetch s at 0x480+24s. Vertex fetch for stream n at 0x778-8n (slot 95-n) | SetTexture `24*(s+48)`, SetStreamSource `8*(239-n)` |
| +0x780 | VS float constants, 256 × float4 | |
| +0x1780 | PS float constants, 256 × float4 | |
| +0x2780 | bool constants (flushed to 0x4900) | |
| +0x2880 | GPU_DESTINATIONPACKET m_DestinationPacket: SurfaceInfo 0x2880, Color0Info 0x2884, DepthInfo 0x2888, Color1Info 0x288C, ... | SetRenderTarget, SetDepthStencilSurface, HighPrecisionBlend setters |
| +0x2898 | memexport stream address (BeginExport) | |
| +0x28C0 | GPU_WINDOWPACKET: WindowOffset; +0x28C4/+0x28C8 WindowScissorTL/BR (packed y<<16\|x) | SetScissorRect |
| +0x28CC | GPU_VALUESPACKET m_ValuesPacket (MaxVtxIndx/MinVtxIndx, IndxOffset 0x28D4, ...) | flush `sub_829EC4D8(dev,0,0x2100,dev+0x28CC)` |
| +0x2901..0x2903 | stencil writemask/mask/ref bytes (Kameo 0x2D41..43 - 0x440) | inferred by shift |
| +0x2904 | AlphaRef (float) | 0x210E in the 0x2100 block |
| +0x2908..0x291C | VportXScale, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET | sub_829E0DC8 writes w/2, w/2+x, -h/2, h/2+y, maxz-minz, minz |
| +0x2920 | 0x2180 block | flush |
| +0x2934 | **DepthControl** (0x2200) | flush `0x2200 ← dev+0x2934`; UE3 RHISetDepthState 0x8297C9C8/0x8297CA88 write it |
| +0x2938 | **BlendControl0**; RT1-3 at 0x2958/0x295C/0x2960 | AlphaBlendEnable writes 0x10001 to all four |
| +0x293C | ColorControl (alpha test/func, alpha-to-mask) | Kameo 0x2D7C - 0x440 |
| +0x2948 | ModeControl (cull bits 0-2, poly-offset enables bits 11/12) | DepthBias setters |
| +0x2964 | GPU_TESSELLATORPACKET m_TessellatorPacket | Kameo 0x2DA4 - 0x440 |
| +0x29B8 | 0x2300 block | flush |
| +0x2A50..0x2A5C | polygon offset front scale/offset, back scale/offset (scale already ×16) | SlopeScaleDepthBias/DepthBias |
| **+0x2B10** | **snooped readback block ptr**: [0] = last GPU-retired fence, [1] = segment position, +8 = system command buffer GPU id (VdSetSystemCommandBufferGpuIdentifierAddress), +0x3C = primary ring RPtr writeback | BlockOnFence, IsFencePending, SetRingBufferParameters |
| +0x2B14 | non-snooped block ptr (+4 written by the present-interval MEM_WRITE packets) | GPU-hang dump label "NonSnooped" |
| **+0x2B1C** | **CPU fence counter** (next fence; += 2 per KickOff; queries stamp it) | InsertSegmentFence, Query_Issue |
| +0x2B20 | busy-tag mask ANDed with resource Common to decide whether a released resource needs a fence | SetIndices, SetTexture |
| +0x2B3C..0x2B40 | device flag bytes (0x2B3C bit 0x20 = predicated tiling active, bit 0x10 = ...; 0x2B3D bit 0x20 = ring allocation failed, bit 4 = HiZ dirty after Clear) | |
| +0x2B50 | primary ring write index | WritePrimary |
| +0x2FD0 | **current vertex declaration** (`D3DVertexDeclaration*`; SetVertexDeclaration is inlined) | IsSet type 5; UE3 RHISetBoundShaderState 0x8297BBD0 |
| +0x2FF8 | blend shadow (SrcBlend bits 0-4, DestBlend 8-12, ...) | |
| +0x2FFC | flags: bit31 AlphaBlendEnable, bit30 SeparateAlpha, bits17-19 (sub_829E27B0), **bits23-29 PresentInterval** | SynchronizeToPresentationInterval `extrwi 7,2`; UE3 writes it at the end of XePresent 0x8297FAE0 |
| +0x3000 | ScissorTestEnable | |
| +0x3014/+0x3018 | ZEnable / ... shadows | SetDepthStencilSurface |
| +0x3044+s / +0x305E+s | per-sampler MaxMipLevel / MinMipLevel bytes | |
| +0x30D2 + byte(+0x3113) | per-surface byte table read by every draw; the low 6 bits go into the DRAW_INDX `vis`/source dword | draw functions |
| **+0x320C** | **index buffer** | SetIndices |
| **+0x3210..0x321C** | **render targets 0-3** | SetRenderTarget |
| **+0x3220** | **depth stencil** | |
| **+0x3224+4n** | **stream n `D3DVertexBuffer*`** (16) | SetStreamSource; IsSet type 1 |
| +0x3268+n | stream n stride in dwords (byte) | |
| **+0x3278+4s** | **bound textures**, 26 slots | SetTexture; UnsetAll loop to 0x1A |
| +0x32E0..0x32F4 | viewport as floats X,Y,W,H,MinZ,MaxZ; +0x32F8 = 0 | sub_829E0DC8 |
| +0x32FC..0x3308 | scissor RECT | SetScissorRect |
| **+0x330C** | **pixel shader** | |
| **+0x3310** | **vertex shader** | |
| +0x331C | predication / tile select mask | SetPredication |
| +0x3324 | tiling-pass active; +0x3330..0x3340 tiling RT/DS snapshot; +0x3344 tile count; +0x3348.. tile rects (16 bytes each); +0x34F4/+0x34F8 tiling surface max W/H | BeginTiling |
| +0x3520 | command-buffer recording pointer (KickOff skips the fence/segment path while it is set) | KickOff `*(dev+13600)` |
| **+0x3614** | **BeginVertices resume ptr** (EndVertices restores +0x30 from here); +0x3618 vertex data, +0x361C index data, +0x3620 / +0x3624 dword counts | BeginVertices / BeginIndexedVertices |
| +0x3650 | memexport nesting counter | |
| +0x3660 | custom ring allocator object (callbacks at +0xAC / +0xB4) | |
| +0x36B0 | refresh class used by present interval (0-1 → 1, 2 → 2, 4 → 3) | the code right before the 0x829EAA14 hook |
| +0x36DC | last scaler params (28 bytes) | Swap |
| +0x3B90 | front-buffer texture copy (52 bytes) | Swap |
| +0x3BC4 / +0x3BC8 | primary ring base / (size>>2)-1 mask | SetRingBufferParameters |
| +0x3BCC / +0x3BD0 / +0x3BD4 | secondary ring base / end / segment size | |
| +0x3C30 | 1536-byte gamma PWL table | SetGammaRamp |
| +0x42DC | pending display-change flags consumed by Swap (2 = gamma) | |
| +0x4248 | frame (swap) counter | Swap `++`, hang dump "frame %i" |

**Resource objects.** D3DResource header: `Common` +0 (type in &0xF, 0x40000000 = child-of-parent, lock count in bits 8+ as +0x100 steps), `ReferenceCount` +4, `Fence` +8, `ReadFence` +0xC, `Identifier` +0x10, `BaseFlush` +0x14 (0xFFFF0000 for buffers).

| object | layout |
| --- | --- |
| Vertex buffer (32 bytes) | +0x18 = address\|3 (vertex fetch dword0), +0x1C = size bytes \| 0x10000002 |
| Index buffer | +0x18 = address, +0x1C = size; Common bit 31 = 32-bit indices (DrawIndexedVertices takes `StartIndex*4` when `*IB < 0`) |
| Texture (52 bytes) | GPU fetch constant = 6 dwords at +0x1C..+0x33 (dword1 at +0x20 = base address, dword5 at +0x30 = mip address) |
| Surface (48 bytes) | +0x1C = RB_*_INFO (EDRAM base\|format), +0x20 = flags, +0x24 = size word (`Width = (v>>18)+1`, `Height = ((v>>3)&0x7FFF)+1`, per D3DSurface_GetDesc); child surfaces: parent +0x18, face<<22 \| level<<28 at +0x1C |
| Shaders | VS 872-byte header, PS 40-byte header; the register patch table is replayed into dev+0x480 |

**UE3 globals:**

| address | contents |
| --- | --- |
| 0x8332FDDC | `GDirect3DDevice` |
| 0x8332FE10 | back-buffer surface |
| 0x8332FE14 | front-buffer texture (Resolve destination and Swap argument) |
| 0x8332FE18 | second surface created at init |
| 0x832B454C / 0x832B4550 | back-buffer W/H |
| 0x832B4558 | present interval UE3 writes into dev+0x2FFC after each Swap |

---

## (e) Frame loop / present path, UE3 RHI layer, readback consumers

### Present path (render thread)

1. Render command `FEndDrawingViewport::Execute` at **0x82488BF8** (vtable entry 0x820F3E84).
2. → **0x82987850** `RHIEndDrawingViewport(bPresent, bLockToVsync)`: sets RT0 to the back buffer and DS to NULL, unbinds everything, then runs deferred releases (0x829823C8: block-on-fence, then free).
3. → **0x8297FAE0** `XePresent` (it also handles the safe-area scaling):
   1. `D3DDevice_SynchronizeToPresentationInterval(dev,0)`, the midasm hook at 0x829EAA14.
   2. `D3DDevice_Resolve(dev, 0, NULL, FrontBufferTex, NULL, 0, NULL, 0.0)`.
   3. `D3DDevice_Swap(dev, FrontBufferTex, pScaler|NULL)`, with VdSwap at 0x829EBA3C (midasm hook).
   4. Finally UE3 rewrites the PresentInterval bits in dev+0x2FFC.

Other callers of 0x8297FAE0:

| caller | role |
| --- | --- |
| 0x8297F5E0 | `XeInitD3DDevice`: CreateDevice + back/front buffers + first present |
| 0x8297A070 | boot/splash image blit + present (loads a texture with D3DX 0x82BA4638, DrawVerticesUP of a QUADLIST) |

Device init:

- 0x8297F5E0 → `Direct3D_CreateDevice`
- 0x8297FD18 → `SetRingBufferParameters`
- 0x8297FE90 `RHISetGammaRamp` → `SetGammaRamp`

### UE3 XeD3DDrv functions (alternative hook layer; names inferred from call shape)

The engine does not call these RHI functions ad hoc. It **records render commands and plays them back**: **0x82782340** is the mesh-element executor (fields +0xB0 start, +0xB4 count, +0xD0 flags). It is the only caller of both 0x8297D438 and 0x8297D3A0, and it inlines EndDrawPrimitiveUP.

#### Draw

| address | function | calls into D3D |
| --- | --- | --- |
| 0x8297D438 | `RHIDrawIndexedPrimitive(IB, Prim, StartIndex, NumPrims)` | SetIndices, per-instance SetStreamSource, DrawIndexedVertices. Contains the XeD3DCommands.cpp:1035 check |
| 0x8297D3A0 | `RHIDrawPrimitive` | DrawVertices / DrawTessellatedVertices |
| 0x8297D660 | `RHIBeginDrawPrimitiveUP` | |
| 0x8297DC48 | `RHIDrawPrimitiveUP` | |
| 0x8297DD20 | `RHIBeginDrawIndexedPrimitiveUP` | |
| 0x8297E198 | `RHIDrawIndexedPrimitiveUP` | |

#### Render targets

| address | function |
| --- | --- |
| 0x8297E2A8 | `RHIClear` (inlined Clear → ClearF) |
| 0x8297CEF8 | `RHISetRenderTarget` (RT + DS + viewport) |
| 0x8297E578 | SetRenderTarget/Depth variant |
| 0x82980D30 | **`RHICopyToResolveTarget`** (Resolve ×5; 135 callers) |
| 0x8297E640 / 0x8297E8A8 | `RHIBeginTiling` / `RHIEndTiling` (predicated tiling; EndTiling resolves ×4) |
| 0x8297F3B8 | BlockUntilIdle wrapper |

#### State

| address | function |
| --- | --- |
| 0x8297B8F8 | `RHISetStreamSource` |
| 0x8297BA28 | `RHISetViewport` |
| 0x8297BAF8 | `RHISetScissorRect` |
| 0x8297BBD0 | `RHISetBoundShaderState` (VS, PS, inlined vertex declaration) |
| 0x8297BCB8 | `RHISetSamplerState` (SetTexture + sampler setters) |
| 0x8297C588 / 0x8297C6C8 / 0x8297C808 | `RHISetVertexShaderParameter` / `RHISetPixelShaderParameter` / vertex float array |
| 0x8297CCD8 / 0x8297CDF0 | `RHISetBlendState` (blend setters + SetBlendState 0x829E3E58) |
| 0x82983560 / 0x829836F8 | `RHISetRasterizerState` (depth bias) |
| 0x8297C9C8 / 0x8297CA88 | `RHISetDepthState` (direct DepthControl writes) |

#### Resources

| address | function |
| --- | --- |
| 0x82984518 / 0x82985110 | `RHICreateTexture2D` / `RHICreateTextureCube` → D3DDevice_CreateTexture |
| 0x8297E390 / 0x82980550 / 0x82980778 | `RHICreateTargetableSurface` and its variants → D3DDevice_CreateSurface |
| 0x82986DC0 / 0x82980018 | `RHICreateVertexBuffer` / `RHICreateIndexBuffer` (physical alloc + XGSet*BufferHeader; there is **no** D3DDevice_CreateVertexBuffer/CreateIndexBuffer) |
| 0x82980208 / 0x82986FE8 | `RHILockVertexBuffer` / `RHILockIndexBuffer` → 0x829DF738 |
| 0x82984988 | `RHIUpdateTexture2D` (CPU write via LockRect) |
| 0x82982768 / 0x82982950 / 0x82982BB8 | shader create (XGRegisterVertexShader / XGRegisterPixelShader / D3DVertexShader_Bind) |
| 0x82979F80 / 0x82986858 / 0x829873E8 | vertex declaration create (0x829E6C28) |

Textures created through the UE3 texture pool use **XGSetTextureHeader** (0x82A01EF0) on pool memory: callers 0x82952478, 0x82984080, 0x82984150, 0x82985DC8. So D3DDevice_CreateTexture does **not** see every texture. Discover textures from the fetch constants at SetTexture/draw time.

#### Direct D3D users outside the RHI

These game-side functions call BeginVertices with an inlined End:

- 0x823AE008 (via 0x829811C8/0x829814D0)
- 0x8248AF80
- 0x8277F3D0
- 0x827D8F20
- 0x8230A288
- 0x82949xxx-0x82958xxx (a UI renderer family; Scaleform-like): 0x829498C0, 0x8294E6D0, 0x8294FE10, 0x829501A0, 0x82956D98, 0x829575F0, 0x82957D68, 0x82958130

### Readback consumers (hook these game functions directly)

1. **`RHIReadSurfaceData` 0x829809C0** (surface, MinX, MinY, MaxX, MaxY, TArray<FColor>&). It creates a temporary A8R8G8B8 texture (0x82984518), calls RHICopyToResolveTarget (0x82980D30), GetSurfaceLevel, LockRect (0x829E5210), XGGetTextureDesc + **XGUntileSurface**, copies rows, unlocks.
   - Callers: `FViewport::ReadPixels` 0x82487760 (direct) and its render-command execute 0x82487920.
   - Chain: 0x82487978 ← 0x82487A70 ← {0x82488410 ← 0x82488D28 (← 0x82533400, 0x8253A7E8), 0x82618CE8}.
   - Screenshot / thumbnail capture. Hook 0x829809C0 and fill OutData from the host RT.
2. **Occlusion queries.** `RHIGetOcclusionQueryResult` **0x829802E0** → `D3DQuery_GetData` (0x829EF248) reads GPU-written ZPASS counters from memory.
   - Callers: 0x8263AC70 (← vtable 0x82125F9C), 0x827D8B10 (← 0x827FD7A8, 0x827FF088), 0x827EDEA0 (← 0x827DC558).
   - Queries are created by 0x827D8178 → `D3DDevice_CreateQuery` and issued at 0x827D8BD8 / 0x827D8F20 → `D3DQuery_Issue`.
   - Hook Issue/GetData as reblue does, and return 1 (pending) or a host query result.
3. **GPU memmove via memexport.** **0x82986B18** `XeGPUMemMove(dst, src, size)`: it binds src as a VB, sets the export stream constant to dst, then loops `BeginExport` / `DrawVertices(POINTLIST, size/64)` / `EndExport`.
   - Callers: 0x82984F50 (← 0x823D37D0 ← 0x823D3148 ← 0x82491AF8) and 0x82985DC8 (vtable 0x8218CA78).
   - This is the texture-pool defragmenter/relocator. **A native renderer must replace it with a CPU memmove** and invalidate host copies of textures in [dst, dst+size) and [src, src+size).
4. **CPU reads of D3D-written memory:**
   - `D3DSurface_LockRect` / `D3DVolume_LockBox` from D3DX (0x82BABC90, 0x82BABEE8, 0x82BAC410). These are load-time paths, not readback.
   - `D3DDevice_IsFencePending` users that poll GPU progress: 0x82982268 (resource busy check), 0x82982610 (deferred free). The hook must make fences retire immediately, since `*(dev+0x2B10)[0]` is written by the GPU's EVENT_WRITE_SHD.
5. **No luminance/auto-exposure CPU readback was found.** All `LockRect` callers are listed above, and no other function in the game locks a resolved target. Eye adaptation therefore stays on the GPU.

---

## (f) Open questions

1. **EndVertices is inlined (+0x3614 → +0x30) at about 20 game sites.** A BeginVertices/BeginIndexedVertices hook has no End callback. Two choices:
   - submit the pending UP draw at the next D3D hook entry or at Swap, or
   - hook the RHI wrappers (0x8297D660/0x8297DC48/0x8297DD20/0x8297E198 plus 0x82782340) and the direct callers listed in (e).
2. **SetRenderState, SetSamplerState and SetVertexDeclaration are mostly inlined.** State must be read from the shadow at draw time. The register map relies on the "Kameo - 0x440" relation, which is verified for scissor, DepthControl, blend, poly offset and viewport. ColorControl (0x293C), ModeControl (0x2948), stencil bytes and point size are inferred by that shift. Spot-check them by setting a known state.
3. **Clone signatures.** Resolve r8 was identified as DestSliceOrFace (not DestLevel) because it is aligned to 8 for 3D textures and passed as the slice argument of sub_829E4CD0 while the level argument is 0. Confirm by logging live calls. ClearF has no stencil argument (it is the constant 0).
4. `D3DDevice_SynchronizeToPresentationInterval`'s r4: UE3 passes 0; Swap's internal path (0x829EACA0) passes the front-buffer GPU address so the flip happens in the vblank ISR. Confirm before replacing both midasm hooks.
5. 0x829E8940/0x829E8B70 versus Kameo's `RingBufferDeviceAllocate`/`BeginRingAlloc`: the roles are clear, the XDK names are not. They are not needed if Swap, the draws and KickOff are all hooked.
6. 0x82986B18's argument order (dst = r3, src = r4, size = r5) was inferred from the export-constant setup. Verify on a live call before relying on the overlap direction.
7. The render-command playback (0x82782340, ~8 callers) may be a better single choke point for meshes than D3D. Its record layout (+0x00 IB wrapper, +0x08 VB, +0x0C/+0x0E strides, +0xB0/+0xB4/+0xB8/+0xBC ranges, +0xD0 flags) still needs mapping.
8. Not found in the binary: D3DDevice_Present, Reset, CreateVertexBuffer, CreateIndexBuffer, CreateVertexDeclaration (the XG-style core 0x829E6C28 is used instead), SetVertexDeclaration, EndVertices, InsertFence (folded into KickOff 0x829E95E8). They are either inlined or unused.
