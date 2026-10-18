// The native renderer: a plume-backed rex::system::IGraphicsSystem injected through RuntimeConfig::graphics; there is no guest GPU, draws reach it through REX_HOOKs on the game's D3D entry points.
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/system/interfaces/graphics.h>

#include "gpu/draw/pipeline_state.h"
#include "gpu/textures/texture_cache.h"
#include "gpu/upload_ring.h"

namespace plume {
struct RenderInterface;
struct RenderDevice;
struct RenderCommandQueue;
struct RenderSwapChain;
struct RenderCommandList;
struct RenderCommandFence;
struct RenderCommandSemaphore;
struct RenderFramebuffer;
struct RenderShader;
struct RenderPipeline;
struct RenderPipelineLayout;
struct RenderDescriptorSet;
struct RenderTexture;
struct RenderTextureView;
struct RenderSampler;
struct RenderBuffer;
}  // namespace plume

struct ShaderCacheEntry;

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace cherry::gpu {

enum class Backend { Auto, D3D12, Vulkan };

struct DrawFrame;
struct DrawRecord;
struct HostGeometry;
struct SurfaceBinding;

inline constexpr uint32_t kFramesInFlight = 2;
// Each frame's upload slice starts at this and grows to what a heavy frame (an area loading) needed, then shrinks back once frames are light again.
inline constexpr uint64_t kMinUploadBytes = 64ull << 20;
inline constexpr uint64_t kMaxUploadBytes = 1024ull << 20;
inline constexpr uint32_t kSwapChainTextureCount = 3;

void* NativeWindowHandle(rex::ui::Window* window);
Backend ConfiguredBackend();

// An ImGui overlay vertex, its position already in clip space.
struct UiVertex {
  float x, y;
  float u, v;
  float r, g, b, a;
};

class GraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  // The window does not exist yet when RuntimeConfig::graphics is filled in, so its handle is fetched lazily.
  GraphicsSystem(std::function<void*()> get_native_window, Backend backend);
  ~GraphicsSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher, rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return device_ != nullptr; }
  void Shutdown() override;

  // From D3DDevice_Swap: hands the captured frame to the render thread, which replays and presents it while the guest builds the next one.
  // Waits while the render thread is still on the previous frame, which paces the guest: the render thread holds each present to the frame rate cap. `frame` comes back empty.
  void SubmitFrame(DrawFrame& frame, const TextureFetch& front_buffer);

  // Debug: hands `done` the next submitted frame's front buffer at the guest's size, top-down BGRA8, on the render thread; null pixels when nothing was shown.
  using FrameCapture = std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch)>;
  void CaptureNextFrame(FrameCapture done);

  // The translated module for a cache entry on the active backend, built on first use; null when the entry has no blob for this backend or it failed to load.
  plume::RenderShader* ShaderModule(const ShaderCacheEntry* entry);

  // The active system, for the hooks; null before SetupPresentation.
  static GraphicsSystem* instance();
  // The width over the height of the picture the game presents: 16:9 on the console, wider with cherry_gfx_ultrawide.
  float PictureAspect() const { return picture_aspect_.load(std::memory_order_relaxed); }

  // The ImGui overlays, built on the UI thread (gpu/ui/imgui_drawer.cpp) and drawn over each presented image by the render thread (gpu/ui/ui_pass.cpp).
  struct UiDraw {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t texture_id = 0;
    int32_t scissor_x = 0;
    int32_t scissor_y = 0;
    int32_t scissor_w = 0;
    int32_t scissor_h = 0;
  };
  struct UiFrame {
    std::vector<UiVertex> vertices;
    std::vector<UiDraw> draws;
  };
  // Registers an RGBA8 texture for UiDraw::texture_id from any thread; it is uploaded at the next present.
  uint32_t CreateUiTexture(uint32_t width, uint32_t height, const uint8_t* rgba);
  // Replaces the overlay drawn at each present; the last one is drawn again until a newer one arrives, so the game never waits for the UI.
  void SubmitUi(UiFrame&& frame);
  // Called on the render thread after every present to ask for the next overlay frame; it must not block.
  void SetUiFrameRequest(std::function<void()> request);
  // Where pipelines.bin lives; set once at startup.
  void SetPipelineCachePath(const std::filesystem::path& path);

 private:
  struct FrameSlot {
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderCommandFence> fence;
    std::unique_ptr<plume::RenderCommandSemaphore> acquire;
    // The cached geometry this frame drew, kept alive until its fence.
    std::vector<std::shared_ptr<HostGeometry>> geometry;
    bool pending = false;
  };

  // Replays the frame into its render targets, shows the texture the guest swapped (`front_buffer`, its fetch constant) and presents; on the render thread.
  void Present(DrawFrame& frame, const TextureFetch& front_buffer);
  void RenderLoop();
  void StopRenderThread();

  bool CreateInterface();
  bool EnsureSwapChain();
  bool CreateFrameResources();
  bool DropStaleSwapChain();
  void ReleaseSwapChain();
  // plume's waitForCommandFence blocks forever on a second wait for one submission, hence the per-slot pending flag.
  void WaitForAllFrames();
  void LimitFrameRate();
  // With vsync on D3D12: waits for the display refresh that keeps every frame on screen for the same whole number of refreshes, the
  // number nearest the frame rate cap (three at 99 Hz, so 33 fps). False when it cannot, and LimitFrameRate paces instead.
  bool WaitForPresentSlot();

  // draw/replay.cpp
  bool EnsureDrawResources(plume::RenderCommandList* list);
  void PrepareTextures(plume::RenderCommandList* list, DrawFrame& frame);
  // Uploads the frame's newly cached geometry into its own GPU buffers.
  void PrepareGeometry(plume::RenderCommandList* list, DrawFrame& frame, std::vector<std::shared_ptr<HostGeometry>>& keep);
  // Copies the vertices of draws whose vertex shader fetches at computed indices into this frame slot's vertex atlas.
  void PrepareVertexAtlas(plume::RenderCommandList* list, DrawFrame& frame);
  void ReplayFrame(plume::RenderCommandList* list, DrawFrame& frame);
  // Logs why the D3D12 device was removed, once; nothing while it is healthy.
  void LogDeviceRemoved();
  // `color_formats` holds one byte per bound render target, 0 past the last.
  plume::RenderPipeline* DrawPipeline(const DrawRecord& record, uint32_t color_formats, uint32_t depth_format);
  PipelineState DescribePipeline(const DrawRecord& record, uint32_t color_formats, uint32_t depth_format) const;
  // Thread-safe: the precompile threads call it as the render thread does.
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const PipelineState& state);
  static uint64_t PipelineKey(const PipelineState& state);
  // Appends a pipeline built for the first time to pipelines.bin.
  void RecordPipeline(const PipelineState& state);
  // Once the draw path is up and the path is known, builds every pipeline pipelines.bin lists on background threads.
  void StartPipelinePrecompile();
  void StopPipelinePrecompile();
  // Draws the resolved front buffer onto the bound swap chain image, letterboxed to its aspect; false when the guest never resolved one.
  bool BlitFrontBuffer(plume::RenderCommandList* list, const TextureFetch& front_buffer, uint32_t width, uint32_t height);

  // ui/ui_pass.cpp
  struct UiTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // until uploaded
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderDescriptorSet> descriptors;
  };
  bool EnsureUiPipeline();
  void UploadUiTextures(plume::RenderCommandList* list);
  // Draws the latest overlay frame onto the swap chain image `target`, after everything else the present records into it.
  void DrawUi(plume::RenderCommandList* list, plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height);
  void RequestUiFrame();
  void ReleaseUi();

  // debug/frame_capture.cpp: CaptureNextFrame's front buffer, drawn 1:1 into its own target and read back.
  bool RecordFrameCapture(plume::RenderCommandList* list, const TextureFetch& front_buffer);
  void DeliverFrameCapture(bool recorded);
  // Under RenderDoc, a frame with a capture request is also recorded as a RenderDoc capture of this device alone.
  void* RenderDocDevice() const;

  // draw/targets.cpp
  struct HostTarget {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t format = 0;  // plume::RenderFormat
    uint32_t width = 0;  // the guest surface's size
    uint32_t height = 0;
    uint32_t host_width = 0;  // the host texture's: the guest size at `scale`
    uint32_t host_height = 0;
    float scale = 1.0f;  // TargetScale of its guest size
    bool depth = false;
    uint32_t layout = 0;  // plume::RenderTextureLayout it was last transitioned to
    uint32_t heap_index = 0;  // the 2D heap descriptor depth and scaled colour resolves read it through
    uint64_t last_write = 0;  // GraphicsSystem::target_writes_ when a draw or clear last wrote it
  };
  // Render targets 0..3, contiguous from 0; null past the last bound one.
  using ColorTargets = std::array<HostTarget*, 4>;
  HostTarget* Target(const SurfaceBinding& surface, bool depth);
  plume::RenderFramebuffer* TargetFramebuffer(const ColorTargets& colors, HostTarget* depth);
  // The draw's render targets 0..3, stopping at the first unbound one.
  ColorTargets DrawTargets(const DrawRecord& record);
  void TransitionTarget(plume::RenderCommandList* list, HostTarget* target, uint32_t layout);
  // Draws a depth target's region into the R32F texture a depth resolve targets; false when it cannot.
  bool ResolveDepth(plume::RenderCommandList* list, const DrawRecord& record, HostTarget* depth);
  // Draws a colour target's region, multiplied by `scale`, into the texture a resolve with an exponent bias targets; false when it cannot.
  // `samples` is the block of `source` texels each resolved pixel averages: an MSAA surface's sample grid when `source` is the single-sample surface it aliases.
  bool ResolveColorScaled(plume::RenderCommandList* list, const DrawRecord& record, HostTarget* source, float scale, uint32_t samples_x = 1, uint32_t samples_y = 1);
  // The single-sample colour target an MSAA surface's EDRAM tiles belong to (same base and host format, size times the sample grid), if it exists.
  HostTarget* MsaaAlias(const SurfaceBinding& surface, uint32_t* samples_x, uint32_t* samples_y);

  std::function<void*()> get_native_window_;
  Backend backend_;
  bool using_vulkan_ = false;
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  void* swap_chain_window_ = nullptr;
  bool swap_chain_failed_ = false;
  FrameSlot frames_[kFramesInFlight];
  std::unique_ptr<plume::RenderCommandSemaphore> present_semaphores_[kSwapChainTextureCount];
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  uint32_t frame_slot_ = 0;
  uint64_t frame_index_ = 0;
  std::chrono::steady_clock::time_point fps_cap_deadline_{};
  // The output the swap chain presents to (an IDXGIOutput, D3D12 only) and when the frame being presented is due on it.
  void* vblank_output_ = nullptr;
  std::chrono::steady_clock::time_point present_due_{};
  std::chrono::nanoseconds refresh_period_{0};  // measured from the output's vblanks, since plume reports no D3D12 refresh rate
  // The bindless layout the translated shaders dictate, with placeholder textures at descriptor 0 until the texture path exists.
  std::unique_ptr<plume::RenderPipelineLayout> draw_layout_;
  std::unique_ptr<plume::RenderDescriptorSet> draw_sets_[4];
  std::unique_ptr<plume::RenderTexture> placeholder_textures_[3];
  std::unique_ptr<plume::RenderTextureView> placeholder_views_[3];
  std::unique_ptr<plume::RenderBuffer> placeholder_upload_;
  std::unique_ptr<plume::RenderSampler> placeholder_sampler_;
  bool placeholders_uploaded_ = false;
  bool draw_ready_ = false;
  bool draw_failed_ = false;
  // One upload buffer per frame in flight, holding that frame's vertices, indices and constant blocks.
  std::unique_ptr<plume::RenderBuffer> upload_[kFramesInFlight];
  uint8_t* upload_mapped_[kFramesInFlight] = {};
  uint64_t upload_bytes_[kFramesInFlight] = {};
  // The largest demand (overflow included) a slice has not yet grown to, and the run of frames that needed under a quarter of the smallest slice with its peak.
  uint64_t upload_grow_ = 0;
  uint32_t upload_quiet_frames_ = 0;
  uint64_t upload_quiet_peak_ = 0;
  // (Re)creates a frame slot's upload slice at `bytes`; false when that fails.
  bool CreateUploadBuffer(uint32_t slot, uint64_t bytes);
  // Sizes the slot about to be used from recent demand; its previous frame has finished on the GPU.
  void FitUploadBuffer(uint32_t slot);
  // Draw pipelines by PipelineKey; the precompile threads add to it too.
  std::atomic<float> picture_aspect_{16.0f / 9.0f};
  std::mutex pipeline_mutex_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  // pipelines.bin: every pipeline built, so the next launch builds them ahead (src/gpu/draw/pipeline_cache.cpp); guarded by pipeline_mutex_.
  std::filesystem::path pipeline_cache_path_;
  std::unordered_set<uint64_t> recorded_pipelines_;
  bool precompile_started_ = false;
  std::atomic<bool> precompile_stop_{false};
  std::vector<std::thread> precompile_threads_;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool>, HostTarget> targets_;
  std::map<std::pair<ColorTargets, const HostTarget*>, std::unique_ptr<plume::RenderFramebuffer>> target_framebuffers_;
  std::unique_ptr<plume::RenderShader> blit_vs_;
  std::unique_ptr<plume::RenderShader> blit_ps_;
  std::unique_ptr<plume::RenderPipeline> blit_pipeline_;
  std::unique_ptr<plume::RenderSampler> blit_sampler_;
  std::unique_ptr<plume::RenderShader> depth_resolve_vs_;
  std::unique_ptr<plume::RenderShader> depth_resolve_ps_;
  std::unique_ptr<plume::RenderPipeline> depth_resolve_pipeline_;
  std::unique_ptr<plume::RenderShader> color_resolve_vs_;
  std::unique_ptr<plume::RenderShader> color_resolve_ps_;
  std::map<uint32_t, std::unique_ptr<plume::RenderPipeline>> color_resolve_pipelines_;  // by plume::RenderFormat
  uint32_t pipeline_failures_ = 0;
  uint64_t target_writes_ = 0;
  // cherry_gfx_render_height over 720: what host render targets, and the resolves out of them, are scaled by.
  float resolution_scale_ = 1.0f;
  // Targets smaller than this on either side hold data, not the picture (the 16x16 colour-grading LUT slices, luminance mips), and their shaders compute it from integer pixel positions, so they stay at the guest size.
  static constexpr uint32_t kMinScaledTarget = 32;
  float TargetScale(uint32_t width, uint32_t height) const { return width >= kMinScaledTarget && height >= kMinScaledTarget ? resolution_scale_ : 1.0f; }
  // The scale of the target a draw renders to.
  float DrawScale(const DrawRecord& record) const;
  static int32_t ScaleCoord(int32_t guest, float scale) { return int32_t(std::lround(double(guest) * scale)); }
  // Picks up a change of render resolution: waits for the GPU, then drops every target and resolve so they are recreated at the new size.
  void UpdateResolutionScale(uint32_t window_height);
  // The render thread and the one frame handed to it.
  std::thread render_thread_;
  std::mutex render_mutex_;
  std::condition_variable render_cv_;
  std::unique_ptr<DrawFrame> render_frame_;
  TextureFetch render_front_;
  bool render_pending_ = false;
  bool render_stop_ = false;
  // Per frame slot: the R32G32B32A32 texture VFETCH_INDEXED reads (shader_common.h), 4096 texels a row.
  std::unique_ptr<plume::RenderTexture> vertex_atlas_[kFramesInFlight];
  std::unique_ptr<plume::RenderTextureView> vertex_atlas_view_[kFramesInFlight];
  uint32_t vertex_atlas_index_[kFramesInFlight] = {};
  // The frame being recorded's upload space, shared by texture uploads (before the render pass) and the draws.
  UploadRing ring_;
  TextureCache textures_;
  // debug/screenshot.cpp: the swap chain image copied out for --cherry_debug_screenshot_every.
  std::unique_ptr<plume::RenderBuffer> screenshot_readback_;
  uint64_t screenshot_bytes_ = 0;
  // The frame being presented is part of a bind_debug_frames burst.
  bool burst_frame_ = false;
  // debug/frame_capture.cpp: requested captures move to render_capture_ with the frame they belong to.
  FrameCapture next_capture_;
  FrameCapture render_capture_;
  std::unique_ptr<plume::RenderTexture> capture_target_;
  std::unique_ptr<plume::RenderFramebuffer> capture_framebuffer_;
  std::unique_ptr<plume::RenderBuffer> capture_readback_;
  uint32_t capture_width_ = 0;
  uint32_t capture_height_ = 0;
  std::mutex shader_mutex_;
  std::unordered_map<const ShaderCacheEntry*, std::unique_ptr<plume::RenderShader>> shader_modules_;
  // ui/ui_pass.cpp: ui_mutex_ guards the overlay frame, the texture registry and the request callback, which the UI and render threads share.
  std::mutex ui_mutex_;
  UiFrame ui_pending_;
  bool ui_pending_fresh_ = false;
  UiFrame ui_current_;  // render thread only
  std::map<uint32_t, UiTexture> ui_textures_;
  uint32_t ui_next_texture_ = 1;
  std::function<void()> ui_frame_request_;
  std::unique_ptr<plume::RenderShader> ui_vs_;
  std::unique_ptr<plume::RenderShader> ui_ps_;
  std::unique_ptr<plume::RenderPipelineLayout> ui_layout_;
  std::unique_ptr<plume::RenderPipeline> ui_pipeline_;
  std::unique_ptr<plume::RenderSampler> ui_sampler_;
  bool ui_pipeline_failed_ = false;
  std::unique_ptr<plume::RenderBuffer> ui_vertices_[kFramesInFlight];
  uint64_t ui_vertex_bytes_[kFramesInFlight] = {};
  std::unique_ptr<plume::RenderBuffer> ui_upload_[kFramesInFlight];
  uint64_t ui_upload_bytes_[kFramesInFlight] = {};

  static GraphicsSystem* s_instance;
};

}  // namespace cherry::gpu
