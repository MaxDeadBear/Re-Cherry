// Offsets into the guest D3DDevice (sizeof 0x6080) that the hooks touch; the full layout is in docs/reverse-engineering/guest-d3d.md.
#pragma once

#include <cstdint>

namespace cherry::gpu::dev {

inline constexpr uint32_t kCmdWritePtr = 0x30;
inline constexpr uint32_t kCmdHardEndPtr = 0x34;
inline constexpr uint32_t kCmdSoftEndPtr = 0x38;
inline constexpr uint32_t kGpuReadbackPtr = 0x2B10;
inline constexpr uint32_t kCpuFence = 0x2B1C;
inline constexpr uint32_t kCustomRingAllocator = 0x3660;
inline constexpr uint32_t kVerticesResumePtr = 0x3614;
inline constexpr uint32_t kSecondaryDataCursor = 0x3BD8;
inline constexpr uint32_t kSegmentStart = 0x3BE4;
inline constexpr uint32_t kSegmentDataBytes = 0x3BE8;
inline constexpr uint32_t kPrimaryRingMask = 0x3BC8;
inline constexpr uint32_t kFrameCounter = 0x4248;
// Written by D3DDevice_SetRingBufferParameters (0x829EA1F0) and read by the packet writers and the ring-data carver (sub_829E8B70).
inline constexpr uint32_t kRingFlags = 0x2B3D;           // byte: bit 0x20 set once ring memory ran out
inline constexpr uint32_t kGprAllocation = 0x2B2C;       // two halfwords, 1 and 0x800 after setup
inline constexpr uint32_t kPrimaryWriteIndex = 0x2B50;
inline constexpr uint32_t kPrimaryBase = 0x3BC4;
inline constexpr uint32_t kSecondaryBase = 0x3BCC;
inline constexpr uint32_t kSecondaryLast = 0x3BD0;       // last dword of the secondary ring
inline constexpr uint32_t kSegmentSize = 0x3BD4;
inline constexpr uint32_t kRingPredicationBits = 0x3BDC;
inline constexpr uint32_t kSecondaryStart = 0x3BE0;
inline constexpr uint32_t kSegmentDataLimit = 0x3BEC;    // data bytes a segment takes before the carver forces a KickOff
inline constexpr uint32_t kReadbackRingPtr = 0x4;        // in the readback page: the ring pointer the GPU would start from
inline constexpr uint32_t kReadbackRPtr = 0x3C;          // in the readback page: the primary ring's read-pointer write-back
// The custom ring allocator's fixed data buffer, used by the carver instead of calling D3D_RingBufferDeviceAllocate.
inline constexpr uint32_t kAllocatorBufferBase = 152;
inline constexpr uint32_t kAllocatorBufferSize = 156;
inline constexpr uint32_t kAllocatorBytes = 176;

// Draw state, read at draw time because LTCG inlined most of the setters into UE3.
inline constexpr uint32_t kFetchConstants = 0x480;
// SetStreamSource writes stream n's vertex fetch constant (two dwords, offset folded in) here minus 8n, i.e. fetch constant 95 - n.
inline constexpr uint32_t kStream0VertexFetch = 0x778;
inline constexpr uint32_t kVertexShaderConstants = 0x780;
inline constexpr uint32_t kPixelShaderConstants = 0x1780;
inline constexpr uint32_t kBoolConstants = 0x2780;
// The XDK's own D3DDevice packets (types in the IDB): m_WindowPacket +0x28C0, m_ValuesPacket +0x28CC, m_ControlPacket +0x2934; bit layouts as GPU_* declares them.
inline constexpr uint32_t kWindowScissorTL = 0x28C4;  // GPU_POINT: X bits 0..14, Y bits 16..30; the full target when the scissor test is off
inline constexpr uint32_t kWindowScissorBR = 0x28C8;
inline constexpr uint32_t kColorMask = 0x28DC;            // GPU_COLORMASK: Write0 bits 0..3
inline constexpr uint32_t kStencilRefMaskBack = 0x28FC;   // GPU_STENCILREFMASK for back faces
inline constexpr uint32_t kStencilRefMask = 0x2900;       // GPU_STENCILREFMASK: Ref 0..7, Mask 8..15, WriteMask 16..23
inline constexpr uint32_t kAlphaRef = 0x2904;             // float
inline constexpr uint32_t kViewportScaleOffset = 0x2908;  // VportXScale, VportXOffset, VportYScale, VportYOffset, VportZScale, VportZOffset
inline constexpr uint32_t kDepthControl = 0x2934;         // GPU_DEPTHCONTROL
inline constexpr uint32_t kBlendControl0 = 0x2938;        // GPU_BLENDCONTROL
inline constexpr uint32_t kBlendControl1 = 0x2958;        // GPU_CONTROLPACKET.BlendControl1..3, one per further render target
inline constexpr uint32_t kColorControl = 0x293C;         // GPU_COLORCONTROL: AlphaFunc 0..2, AlphaTestEnable 3
inline constexpr uint32_t kModeControl = 0x2948;          // GPU_MODECONTROL: CullMode 0..2 (UE3 writes 2 for CW, 6 for CCW), PolyMode 3..10
inline constexpr uint32_t kVteControl = 0x294C;           // GPU_VTECONTROL: which Vport* terms apply
inline constexpr uint32_t kPolyOffsetFrontScale = 0x2A50;  // GPU_POINTPACKET.PolyOffsetFrontScale: D3D's SlopeScaleDepthBias x16
inline constexpr uint32_t kPolyOffsetFrontOffset = 0x2A54; // GPU_POINTPACKET.PolyOffsetFrontOffset: D3D's DepthBias, in window depth
inline constexpr uint32_t kVtxControl = 0x29C0;           // GPU_MISCPACKET.VtxControl (GPU_VTXCONTROL): PixCenter bit 0
inline constexpr uint32_t kVertexDeclaration = 0x2FD0;
inline constexpr uint32_t kIndexBuffer = 0x320C;
inline constexpr uint32_t kRenderTarget0 = 0x3210;
inline constexpr uint32_t kDepthStencil = 0x3220;
inline constexpr uint32_t kStreamStrides = 0x3268;  // one byte per stream, stride >> 2
// SetTexture: the bound D3DBaseTexture per sampler; its fetch constant is the six dwords at kFetchConstants + 24 * sampler.
inline constexpr uint32_t kTextureObjects = 0x3278;
inline constexpr uint32_t kPixelShader = 0x330C;
inline constexpr uint32_t kVertexShader = 0x3310;

// D3D_CDevice_StartNewSegment keeps 16 dwords and the end-of-segment packet clear of the hard end, and the soft end 160 bytes under it.
inline constexpr uint32_t kSegmentTailBytes = 68;
inline constexpr uint32_t kSoftEndGapBytes = 160;

}  // namespace cherry::gpu::dev
