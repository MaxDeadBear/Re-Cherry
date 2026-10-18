#include "gpu/graphics_system.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <cmath>
#include <thread>

#include <plume_render_interface.h>

#ifdef _WIN32
#include <plume_d3d12.h>
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/ui/window.h>

#include "engine/frame_rate.h"
#include "gpu/debug/dred.h"
#include "gpu/debug/frame_timing.h"
#include "gpu/debug/renderdoc.h"
#include "gpu/debug/screenshot.h"
#include "gpu/draw/draw_frame.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_registry.h"

REXCVAR_DEFINE_STRING(cherry_gfx_backend, "auto", "Lollipop/Graphics", "Renderer backend: auto (D3D12 on Windows, falling back to Vulkan), d3d12 or vulkan.");
REXCVAR_DEFINE_BOOL(cherry_gfx_vsync, true, "Lollipop/Graphics", "Wait for the display's refresh before presenting.");
REXCVAR_DEFINE_INT32(cherry_gfx_render_height, 0, "Lollipop/Graphics", "Height the game is rendered at: 720 is the console's, 144/360 below it and 1080/1440/2160 above draw everything at that resolution, 0 follows the window.").range(0, 4320);
REXCVAR_DEFINE_INT32(cherry_gfx_anisotropy, 0, "Lollipop/Graphics", "Anisotropic filtering for mipmapped textures: 0 keeps the game's own, 2, 4, 8 or 16 forces that level.").range(0, 16);
REXCVAR_DEFINE_BOOL(cherry_gfx_fxaa, false, "Lollipop/Graphics", "Smooth jagged edges with FXAA on the final image (the HUD included).");
REXCVAR_DEFINE_INT32(cherry_gfx_brightness, 0, "Lollipop/Graphics", "Brightness in steps of a tenth of gamma: -2 darker to +2 brighter, 0 as the game made it.").range(-3, 3);
REXCVAR_DEFINE_INT32(cherry_gfx_sharpen, 0, "Lollipop/Graphics", "Sharpen the final picture: 0 off, 1 low, 2 high; pairs well with FXAA and render resolutions below the window's.").range(0, 2);
REXCVAR_DEFINE_BOOL(cherry_gfx_stretch, false, "Lollipop/Graphics", "Stretch the picture to fill the window instead of keeping its shape with black bars.");

namespace plume {
#ifdef _WIN32
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

// The X_STATUS_* macros cast to an unqualified X_STATUS.
using rex::X_STATUS;

namespace cherry::gpu {

GraphicsSystem* GraphicsSystem::s_instance = nullptr;

GraphicsSystem* GraphicsSystem::instance() { return s_instance; }

namespace {

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;

bool WindowHasArea(void* window) {
#ifdef _WIN32
  RECT rect = {};
  return window && GetClientRect(static_cast<HWND>(window), &rect) && rect.right > rect.left && rect.bottom > rect.top;
#else
  int width = 0;
  int height = 0;
  return window && SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window), &width, &height) && width > 0 && height > 0;
#endif
}

void SleepUntil(std::chrono::steady_clock::time_point deadline) {
  const auto spin_margin = std::chrono::milliseconds(1);
  const auto now = std::chrono::steady_clock::now();
  if (deadline - now > spin_margin) std::this_thread::sleep_for(deadline - now - spin_margin);
  while (std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
}

}  // namespace

// The runtime hands out a native handle only on Windows; elsewhere the single SDL window stands in for it.
void* NativeWindowHandle(rex::ui::Window* window) {
  if (!window) return nullptr;
#ifdef _WIN32
  return window->GetNativeWindowHandle();
#else
  static std::atomic<SDL_Window*> found{nullptr};
  if (SDL_Window* known = found.load()) return known;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* first = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  found.store(first);
  return first;
#endif
}

Backend ConfiguredBackend() {
  const std::string want = REXCVAR_GET(cherry_gfx_backend);
  if (want == "d3d12" || want == "dx12") return Backend::D3D12;
  if (want == "vulkan" || want == "vk") return Backend::Vulkan;
  return Backend::Auto;
}

GraphicsSystem::GraphicsSystem(std::function<void*()> get_native_window, Backend backend) : get_native_window_(std::move(get_native_window)), backend_(backend) {}

GraphicsSystem::~GraphicsSystem() { Shutdown(); }

bool GraphicsSystem::CreateInterface() {
  auto try_d3d12 = [&] {
#ifdef _WIN32
    interface_ = plume::CreateD3D12Interface();
    using_vulkan_ = false;
    return interface_ != nullptr;
#else
    return false;
#endif
  };
  auto try_vulkan = [&] {
    interface_ = plume::CreateVulkanInterface();
    using_vulkan_ = true;
    return interface_ != nullptr;
  };
  switch (backend_) {
    case Backend::D3D12: return try_d3d12();
    case Backend::Vulkan: return try_vulkan();
    default: return try_d3d12() || try_vulkan();
  }
}

rex::X_STATUS GraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  (void)app_context;
  if (device_) return X_STATUS_SUCCESS;
  if (!CreateInterface()) {
    REXGPU_ERROR("[gfx] no usable graphics backend");
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!using_vulkan_) debug::EnableDred();
  device_ = interface_->createDevice();
  if (!device_) {
    REXGPU_ERROR("[gfx] createDevice failed ({})", using_vulkan_ ? "vulkan" : "d3d12");
    return X_STATUS_UNSUCCESSFUL;
  }
  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!queue_) {
    REXGPU_ERROR("[gfx] createCommandQueue failed");
    device_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }
  const plume::RenderDeviceDescription& desc = device_->getDescription();
  REXGPU_INFO("[gfx] native renderer: backend={} device=\"{}\" vram={} MiB", using_vulkan_ ? "vulkan" : "d3d12", desc.name, desc.dedicatedVideoMemory / (1024 * 1024));
  s_instance = this;
  return X_STATUS_SUCCESS;
}

// Deliberately empty: no ring buffer consumer, vsync worker or interrupts, because the hooks replace the guest functions that would wait on them.
rex::X_STATUS GraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher, rex::system::KernelState* kernel_state) {
  (void)function_dispatcher;
  (void)kernel_state;
  REXGPU_INFO("[gfx] guest GPU not emulated");
  return X_STATUS_SUCCESS;
}

void GraphicsSystem::SubmitFrame(DrawFrame& frame, const TextureFetch& front_buffer) {
  const auto wait_start = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lock(render_mutex_);
    if (!render_thread_.joinable()) {
      render_frame_ = std::make_unique<DrawFrame>();
      render_thread_ = std::thread([this] { RenderLoop(); });
    }
    render_cv_.wait(lock, [&] { return !render_pending_; });
    std::swap(*render_frame_, frame);
    frame.Clear();
    render_front_ = front_buffer;
    render_capture_ = std::move(next_capture_);
    next_capture_ = nullptr;
    render_pending_ = true;
  }
  render_cv_.notify_all();
  debug::NoteGuestFrame(std::chrono::steady_clock::now() - wait_start, {});
}

void GraphicsSystem::RenderLoop() {
  for (;;) {
    std::unique_lock<std::mutex> lock(render_mutex_);
    render_cv_.wait(lock, [&] { return render_pending_ || render_stop_; });
    if (render_stop_) return;
    lock.unlock();
    burst_frame_ = debug::TakeBurstFrame();
    const bool renderdoc = (render_capture_ || burst_frame_) && debug::RenderDocBeginDeviceCapture(RenderDocDevice(), swap_chain_window_);
    Present(*render_frame_, render_front_);
    if (renderdoc) debug::RenderDocEndDeviceCapture(RenderDocDevice(), swap_chain_window_);
    if (render_capture_) DeliverFrameCapture(false);
    RequestUiFrame();
    lock.lock();
    render_pending_ = false;
    lock.unlock();
    render_cv_.notify_all();
  }
}

void GraphicsSystem::StopRenderThread() {
  {
    std::lock_guard<std::mutex> lock(render_mutex_);
    render_stop_ = true;
  }
  render_cv_.notify_all();
  if (render_thread_.joinable()) render_thread_.join();
}

void GraphicsSystem::Shutdown() {
  StopRenderThread();
  if (queue_) WaitForAllFrames();
  if (s_instance == this) s_instance = nullptr;
  ReleaseUi();
  ReleaseSwapChain();
  blit_pipeline_.reset();
  blit_vs_.reset();
  blit_ps_.reset();
  blit_sampler_.reset();
  depth_resolve_pipeline_.reset();
  color_resolve_pipelines_.clear();
  color_resolve_vs_.reset();
  color_resolve_ps_.reset();
  depth_resolve_vs_.reset();
  depth_resolve_ps_.reset();
  target_framebuffers_.clear();
  targets_.clear();
  textures_.Shutdown();
  screenshot_readback_.reset();
  capture_framebuffer_.reset();
  capture_target_.reset();
  capture_readback_.reset();
  StopPipelinePrecompile();
  pipelines_.clear();
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    if (upload_mapped_[slot]) upload_[slot]->unmap();
    upload_mapped_[slot] = nullptr;
    upload_[slot].reset();
  }
  for (auto& view : placeholder_views_) view.reset();
  for (auto& texture : placeholder_textures_) texture.reset();
  placeholder_upload_.reset();
  placeholder_sampler_.reset();
  for (auto& set : draw_sets_) set.reset();
  draw_layout_.reset();
  draw_ready_ = false;
  {
    std::lock_guard<std::mutex> lock(shader_mutex_);
    shader_modules_.clear();
  }
  queue_.reset();
  device_.reset();
  interface_.reset();
}

void GraphicsSystem::ReleaseSwapChain() {
  framebuffers_.clear();
  for (auto& semaphore : present_semaphores_) semaphore.reset();
  for (FrameSlot& slot : frames_) {
    slot.acquire.reset();
    slot.fence.reset();
    slot.list.reset();
    slot.pending = false;
  }
#ifdef _WIN32
  if (vblank_output_) static_cast<IDXGIOutput*>(vblank_output_)->Release();
#endif
  vblank_output_ = nullptr;
  present_due_ = {};
  refresh_period_ = std::chrono::nanoseconds(0);
  swap_chain_.reset();
  swap_chain_window_ = nullptr;
}

void GraphicsSystem::UpdateResolutionScale(uint32_t window_height) {
  const int32_t height = REXCVAR_GET(cherry_gfx_render_height);
  const uint32_t render_height = height > 0 ? uint32_t(height) : window_height;
  if (!render_height) return;
  // The console renders 720 lines; everything the replay draws scales from there.
  const float scale = std::clamp(float(render_height) / 720.0f, 0.2f, 6.0f);
  if (scale == resolution_scale_) return;
  WaitForAllFrames();
  for (auto& [key, target] : targets_) textures_.ReleaseIndex(target.heap_index);
  target_framebuffers_.clear();
  targets_.clear();
  textures_.DropResolved();
  resolution_scale_ = scale;
  REXGPU_INFO("[gfx] rendering at {} lines ({:.3}x the console's 720)", render_height, scale);
}

void GraphicsSystem::WaitForAllFrames() {
  for (FrameSlot& slot : frames_) {
    if (!slot.pending) continue;
    slot.pending = false;
    queue_->waitForCommandFence(slot.fence.get());
  }
}

bool GraphicsSystem::EnsureSwapChain() {
  if (swap_chain_) return true;
  if (swap_chain_failed_) return false;
  void* window = get_native_window_ ? get_native_window_() : nullptr;
  if (!WindowHasArea(window)) return false;

  plume::RenderSwapChainDesc swap_desc;
#ifdef _WIN32
  swap_desc.renderWindow = reinterpret_cast<plume::RenderWindow>(window);
#else
  // plume's Vulkan backend only builds Xlib surfaces.
  const SDL_PropertiesID props = SDL_GetWindowProperties(static_cast<SDL_Window*>(window));
  swap_desc.renderWindow.display = static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  swap_desc.renderWindow.window = ::Window(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!swap_desc.renderWindow.display || !swap_desc.renderWindow.window) {
    REXGPU_ERROR("[gfx] the native renderer needs an X11 window; start the game with --video_driver=x11");
    swap_chain_failed_ = true;
    return false;
  }
#endif
  swap_desc.format = kColorFormat;
  swap_desc.textureCount = kSwapChainTextureCount;
  swap_chain_ = queue_->createSwapChain(swap_desc);
  if (swap_chain_ && (swap_chain_->getWidth() == 0 || swap_chain_->getHeight() == 0)) swap_chain_->resize();
  if (!swap_chain_ || swap_chain_->getWidth() == 0 || swap_chain_->getHeight() == 0) {
    swap_chain_.reset();
    return false;
  }
  swap_chain_window_ = window;
  swap_chain_->setVsyncEnabled(REXCVAR_GET(cherry_gfx_vsync));
  if (!CreateFrameResources()) {
    REXGPU_ERROR("[gfx] swap chain frame resources failed");
    ReleaseSwapChain();
    swap_chain_failed_ = true;
    return false;
  }
  REXGPU_INFO("[gfx] swap chain ready ({}x{}, {} buffers)", swap_chain_->getWidth(), swap_chain_->getHeight(), swap_chain_->getTextureCount());
  return true;
}

bool GraphicsSystem::CreateFrameResources() {
  frame_slot_ = 0;
  for (FrameSlot& slot : frames_) {
    slot.pending = false;
    slot.list = queue_->createCommandList();
    slot.fence = device_->createCommandFence();
    slot.acquire = device_->createCommandSemaphore();
    if (!slot.list || !slot.fence || !slot.acquire) return false;
  }
  // One per swap chain image: the presentation engine holds an image's semaphore until that image is acquired again.
  for (auto& semaphore : present_semaphores_) {
    semaphore = device_->createCommandSemaphore();
    if (!semaphore) return false;
  }
  framebuffers_.clear();
  for (uint32_t i = 0; i < swap_chain_->getTextureCount(); ++i) {
    const plume::RenderTexture* color = swap_chain_->getTexture(i);
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    auto framebuffer = device_->createFramebuffer(desc);
    if (!framebuffer) return false;
    framebuffers_.push_back(std::move(framebuffer));
  }
  return true;
}

// plume's resize() faulted on window changes in Kameo, so a stale swap chain is torn down and rebuilt on the next frame instead.
bool GraphicsSystem::DropStaleSwapChain() {
  if (!swap_chain_) return false;
  void* window = get_native_window_ ? get_native_window_() : nullptr;
  if (!WindowHasArea(window) || (window == swap_chain_window_ && !swap_chain_->needsResize())) return false;
  REXGPU_INFO("[gfx] rebuilding the swap chain ({})", window != swap_chain_window_ ? "window handle replaced" : "size changed");
  WaitForAllFrames();
  ReleaseSwapChain();
  swap_chain_failed_ = false;
  return true;
}

void GraphicsSystem::LogDeviceRemoved() {
#ifdef _WIN32
  static bool logged = false;
  if (using_vulkan_ || logged) return;
  const HRESULT reason = static_cast<plume::D3D12Device*>(device_.get())->d3d->GetDeviceRemovedReason();
  if (reason == S_OK) return;
  logged = true;
  REXGPU_ERROR("[gfx] D3D12 device removed at frame {}: {:08X}", frame_index_, uint32_t(reason));
  debug::LogDred(device_.get());
#endif
}

void GraphicsSystem::Present(DrawFrame& draws, const TextureFetch& front_buffer) {
  if (DropStaleSwapChain() || !EnsureSwapChain() || swap_chain_->isEmpty()) {
    LimitFrameRate();
    return;
  }

  FrameSlot& frame = frames_[frame_slot_];
  if (frame.pending) {
    debug::ScopedFrameTimer timer(debug::TimingPhase::FenceWait);
    frame.pending = false;
    queue_->waitForCommandFence(frame.fence.get());
  }
  frame.geometry.clear();

  uint32_t index = 0;
  if (!swap_chain_->acquireTexture(frame.acquire.get(), &index)) {
    LimitFrameRate();
    return;
  }
  plume::RenderTexture* texture = swap_chain_->getTexture(index);
  const uint32_t width = swap_chain_->getWidth();
  const uint32_t height = swap_chain_->getHeight();
  UpdateResolutionScale(height);
  textures_.SetForcedAnisotropy(uint32_t(REXCVAR_GET(cherry_gfx_anisotropy)));

  plume::RenderCommandList* list = frame.list.get();
  list->begin();
  // Before the render pass opens: the first call records the placeholder texture uploads, and the frame's textures upload here too.
  if (EnsureDrawResources(list)) {
    FitUploadBuffer(frame_slot_);
    ring_ = UploadRing{upload_[frame_slot_].get(), upload_mapped_[frame_slot_], upload_bytes_[frame_slot_], 0};
    textures_.BeginFrame(frame_index_);
    {
      debug::ScopedFrameTimer timer(debug::TimingPhase::Textures);
      PrepareTextures(list, draws);
      PrepareGeometry(list, draws, frame.geometry);
      PrepareVertexAtlas(list, draws);
    }
    debug::ScopedFrameTimer timer(debug::TimingPhase::Replay);
    ReplayFrame(list, draws);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(framebuffers_[index].get());
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  list->setScissors(plume::RenderRect(0, 0, width, height));
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  BlitFrontBuffer(list, front_buffer, width, height);
  const bool capture = render_capture_ && RecordFrameCapture(list, front_buffer);
  // The copy's rows are 256-byte aligned, as D3D12 requires of a buffer footprint.
  const uint32_t interval = debug::ScreenshotInterval();
  const bool screenshot = burst_frame_ || (interval && (frame_index_ % interval) == 0);
  const uint32_t row_pitch = (width * 4 + 255) & ~255u;
  if (screenshot) {
    const uint64_t bytes = uint64_t(row_pitch) * height;
    if (!screenshot_readback_ || screenshot_bytes_ < bytes) {
      screenshot_readback_ = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
      screenshot_bytes_ = screenshot_readback_ ? bytes : 0;
    }
    if (screenshot_readback_) {
      list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_SOURCE));
      debug::CopyToReadback(list, texture, screenshot_readback_.get(), width, height, row_pitch / 4);
    }
  }
  // After the screenshot copy, so debug shots show the game alone.
  DrawUi(list, texture, framebuffers_[index].get(), width, height);
  list->barriers(plume::RenderBarrierStage::NONE, plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::PRESENT));
  list->end();

  const plume::RenderCommandList* lists = list;
  plume::RenderCommandSemaphore* wait = frame.acquire.get();
  plume::RenderCommandSemaphore* signal = present_semaphores_[index].get();
  {
    debug::ScopedFrameTimer timer(debug::TimingPhase::Submit);
    queue_->executeCommandLists(&lists, 1, &wait, 1, &signal, 1, frame.fence.get());
  }
  frame.pending = true;

  if (screenshot && screenshot_readback_) {
    frame.pending = false;
    queue_->waitForCommandFence(frame.fence.get());
    if (const uint8_t* pixels = static_cast<const uint8_t*>(screenshot_readback_->map())) {
      debug::WriteTga(std::filesystem::path("shots") / std::format("frame_{:06}.tga", frame_index_), pixels, width, height, row_pitch);
      screenshot_readback_->unmap();
    } else {
      LogDeviceRemoved();
    }
  }
  if (capture) {
    if (frame.pending) {
      frame.pending = false;
      queue_->waitForCommandFence(frame.fence.get());
    }
    DeliverFrameCapture(true);
  }
  debug::RenderDocBeforePresent(frame_index_);
  swap_chain_->setVsyncEnabled(REXCVAR_GET(cherry_gfx_vsync));
  // Paced here, after the frame's variable upload and recording work, so presents keep an even cadence.
  if (!WaitForPresentSlot()) LimitFrameRate();
  {
    debug::ScopedFrameTimer timer(debug::TimingPhase::Submit);
    swap_chain_->present(index, &signal, 1);
  }
  debug::EndFrameTiming();
  frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;
  if ((frame_index_++ % 600) == 0) {
    REXGPU_INFO("[gfx] presented frame {} ({}x{})", frame_index_, width, height);
    LogDeviceRemoved();
  }
}

plume::RenderShader* GraphicsSystem::ShaderModule(const ShaderCacheEntry* entry) {
  if (!entry || !device_) return nullptr;
  std::lock_guard<std::mutex> lock(shader_mutex_);
  auto [found, inserted] = shader_modules_.try_emplace(entry);
  if (inserted) {
    const std::vector<uint8_t> blob = using_vulkan_ ? ShaderSpirv(entry) : ShaderDxil(entry);
    // XenosRecomp emits "main" for both stages.
    if (!blob.empty()) found->second = device_->createShader(blob.data(), blob.size(), "main", using_vulkan_ ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL);
    if (!found->second) REXGPU_WARN("[gfx] shader {:016X} has no loadable {} module", entry->hash, using_vulkan_ ? "SPIR-V" : "DXIL");
  }
  return found->second.get();
}

bool GraphicsSystem::WaitForPresentSlot() {
#ifdef _WIN32
  if (using_vulkan_ || !REXCVAR_GET(cherry_gfx_vsync) || !swap_chain_) return false;
  if (!vblank_output_) {
    IDXGIOutput* output = nullptr;
    if (FAILED(static_cast<plume::D3D12SwapChain*>(swap_chain_.get())->d3d->GetContainingOutput(&output))) return false;
    vblank_output_ = output;
  }
  auto* output = static_cast<IDXGIOutput*>(vblank_output_);
  if (refresh_period_.count() == 0) {
    constexpr int kSamples = 8;
    if (FAILED(output->WaitForVBlank())) return false;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kSamples; ++i) output->WaitForVBlank();
    refresh_period_ = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start) / kSamples;
    if (refresh_period_ < std::chrono::milliseconds(2) || refresh_period_ > std::chrono::milliseconds(50)) return false;
  }
  const auto period = refresh_period_;
  const double hz = 1e9 / double(period.count());
  const int fps = cherry::engine::TargetFps();
  static int logged_fps = -1;
  if (logged_fps != fps) {
    logged_fps = fps;
    REXGPU_INFO("[gfx] presenting on the vblanks of a {:.2f} Hz display at {}", hz, fps > 0 && fps < hz ? std::format("{} fps", fps) : std::string("every refresh"));
  }
  // Each frame is due 1/fps after the last one's due time, not after its vblank, so a rate the refresh does not divide (30 on 75 Hz) alternates 2 and 3 refreshes and averages out exactly; a frame that ran late restarts the schedule.
  const auto now = std::chrono::steady_clock::now();
  if (fps > 0 && fps < hz) {
    const auto interval = std::chrono::nanoseconds(1000000000LL / fps);
    if (present_due_.time_since_epoch().count() == 0 || now > present_due_ + interval) present_due_ = now;
    else present_due_ += interval;
    SleepUntil(present_due_ - period / 2);
  } else {
    present_due_ = {};
  }
  if (FAILED(output->WaitForVBlank())) return false;
  return true;
#else
  return false;
#endif
}

// Holds each present to target_fps, which on the console was the swap's presentation interval.
void GraphicsSystem::LimitFrameRate() {
  if (cherry::engine::TargetFps() == 0) {
    fps_cap_deadline_ = {};
    return;
  }
  const auto interval = std::chrono::nanoseconds(1000000000LL / std::max(cherry::engine::TargetFps(), 1));
  const auto now = std::chrono::steady_clock::now();
  // The first frame, or one that ran past its slot, starts a fresh schedule from now: no extra wait, and no racing to catch up.
  if (fps_cap_deadline_.time_since_epoch().count() == 0 || now > fps_cap_deadline_ + interval) {
    fps_cap_deadline_ = now;
    return;
  }
  fps_cap_deadline_ += interval;
  SleepUntil(fps_cap_deadline_);
}

}  // namespace cherry::gpu
