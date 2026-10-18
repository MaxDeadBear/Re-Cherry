#include "gpu/debug/dred.h"

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <plume_d3d12.h>
#endif

REXCVAR_DEFINE_BOOL(cherry_debug_dred, false, "Lollipop/Debug", "Record D3D12 DRED breadcrumbs and page faults, and log them when the device is removed (D3D12 only; slower).");

namespace cherry::gpu::debug {

#ifdef _WIN32

namespace {

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    default: return "other";
  }
}

bool IsDraw(D3D12_AUTO_BREADCRUMB_OP op) {
  return op == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED || op == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED;
}

}  // namespace

void EnableDred() {
  if (!REXCVAR_GET(cherry_debug_dred)) return;
  ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
    REXGPU_WARN("[dred] DRED settings unavailable");
    return;
  }
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->Release();
  REXGPU_INFO("[dred] breadcrumbs and page faults on");
}

void LogDred(plume::RenderDevice* device) {
  static bool logged = false;
  if (!REXCVAR_GET(cherry_debug_dred) || logged || !device) return;
  logged = true;
  ID3D12DeviceRemovedExtendedData1* dred = nullptr;
  if (FAILED(static_cast<plume::D3D12Device*>(device)->d3d->QueryInterface(IID_PPV_ARGS(&dred)))) return;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
      const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      // Lists the GPU finished entirely are not where it stopped.
      if (done >= node->BreadcrumbCount) continue;
      uint32_t draws = 0;
      for (uint32_t i = 0; i < done; ++i) draws += IsDraw(node->pCommandHistory[i]);
      REXGPU_ERROR("[dred] command list {}: {} of {} operations finished, {} draws before the stop", static_cast<const void*>(node->pCommandList), done, node->BreadcrumbCount, draws);
      const uint32_t first = done > 8 ? done - 8 : 0;
      const uint32_t last = std::min<uint32_t>(node->BreadcrumbCount, done + 8);
      for (uint32_t i = first; i < last; ++i) REXGPU_ERROR("[dred]   {}{} {}", i == done ? "-> " : "   ", i, OpName(node->pCommandHistory[i]));
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
    REXGPU_ERROR("[dred] page fault at GPU VA {:016X}", uint64_t(fault.PageFaultVA));
    for (auto* a = fault.pHeadExistingAllocationNode; a; a = a->pNext) REXGPU_ERROR("[dred]   live allocation {} type {}", a->ObjectNameA ? a->ObjectNameA : "?", int(a->AllocationType));
    for (auto* a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext) REXGPU_ERROR("[dred]   freed allocation {} type {}", a->ObjectNameA ? a->ObjectNameA : "?", int(a->AllocationType));
  } else {
    REXGPU_ERROR("[dred] no page fault: the GPU timed out");
  }
  dred->Release();
}

#else

void EnableDred() {}
void LogDred(plume::RenderDevice*) {}

#endif

}  // namespace cherry::gpu::debug
