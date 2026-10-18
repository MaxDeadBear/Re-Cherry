#include "compare/xenia/paired_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>

#include "gpu/graphics_system.h"

namespace cherry::compare {

namespace {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgra;
};

std::atomic<bool> g_requested{false};
uint64_t g_swap = 0;
std::mutex g_native_mutex;
std::condition_variable g_native_ready;
bool g_native_done = false;
Image g_native;

Image FromXenia(const rex::ui::RawImage& raw) {
  Image image{raw.width, raw.height, std::vector<uint8_t>(size_t(raw.width) * raw.height * 4)};
  for (uint32_t y = 0; y < raw.height; ++y) {
    const uint8_t* src = raw.data.data() + y * raw.stride;
    uint8_t* dst = image.bgra.data() + size_t(y) * raw.width * 4;
    for (uint32_t x = 0; x < raw.width; ++x) {
      dst[x * 4] = src[x * 4 + 2];
      dst[x * 4 + 1] = src[x * 4 + 1];
      dst[x * 4 + 2] = src[x * 4];
      dst[x * 4 + 3] = 0xFF;
    }
  }
  return image;
}

Image SideBySide(const Image& left, const Image& right) {
  Image out{left.width + right.width, std::max(left.height, right.height), {}};
  out.bgra.assign(size_t(out.width) * out.height * 4, 0);
  for (size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 0xFF;
  for (uint32_t y = 0; y < left.height; ++y) std::memcpy(out.bgra.data() + size_t(y) * out.width * 4, left.bgra.data() + size_t(y) * left.width * 4, size_t(left.width) * 4);
  for (uint32_t y = 0; y < right.height; ++y) std::memcpy(out.bgra.data() + (size_t(y) * out.width + left.width) * 4, right.bgra.data() + size_t(y) * right.width * 4, size_t(right.width) * 4);
  return out;
}

bool WritePng(const std::filesystem::path& path, const Image& image) {
  using Microsoft::WRL::ComPtr;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool written = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const UINT stride = image.width * 4;
    written = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA && SUCCEEDED(frame->WritePixels(image.height, stride, UINT(image.bgra.size()), const_cast<BYTE*>(image.bgra.data()))) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return written;
}

}  // namespace

void RequestPairedCapture() {
  g_requested = true;
}

void RegisterCaptureBind() {
  rex::ui::RegisterBind("bind_compare_capture", "F8", "Compare mode: paired Xenia and native screenshot", [] { RequestPairedCapture(); });
}

bool BeginPairedCapture() {
  ++g_swap;
  if (!g_requested.exchange(false)) return false;
  {
    std::lock_guard<std::mutex> lock(g_native_mutex);
    g_native_done = false;
    g_native = {};
  }
  gpu::GraphicsSystem* graphics = gpu::GraphicsSystem::instance();
  if (!graphics) {
    std::lock_guard<std::mutex> lock(g_native_mutex);
    g_native_done = true;
    return true;
  }
  graphics->CaptureNextFrame([](const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) {
    Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (uint32_t y = 0; bgra && y < height; ++y) {
      std::memcpy(image.bgra.data() + size_t(y) * width * 4, bgra + size_t(y) * pitch, size_t(width) * 4);
      for (uint32_t x = 0; x < width; ++x) image.bgra[(size_t(y) * width + x) * 4 + 3] = 0xFF;
    }
    {
      std::lock_guard<std::mutex> lock(g_native_mutex);
      g_native = std::move(image);
      g_native_done = true;
    }
    g_native_ready.notify_all();
  });
  return true;
}

void FinishPairedCapture(const std::function<void()>& wait_for_xenia) {
  const uint64_t swap = g_swap - 1;
  wait_for_xenia();
  rex::ui::RawImage raw;
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* xenia = runtime ? runtime->graphics_system() : nullptr;
  const bool have_xenia = xenia && xenia->presenter() && xenia->presenter()->CaptureGuestOutput(raw);
  Image native;
  {
    std::unique_lock<std::mutex> lock(g_native_mutex);
    if (!g_native_ready.wait_for(lock, std::chrono::seconds(5), [] { return g_native_done; })) REXLOG_WARN("[compare] the native renderer gave no capture for swap {}", swap);
    native = std::move(g_native);
  }
  Image left = have_xenia ? FromXenia(raw) : Image{};
  if (!have_xenia) REXLOG_WARN("[compare] Xenia gave no capture for swap {}", swap);
  // Encoding takes a while; the game carries on meanwhile.
  std::thread([swap, left = std::move(left), native = std::move(native)] {
    std::error_code ec;
    std::filesystem::create_directories("shots", ec);
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::filesystem::path path = std::filesystem::path("shots") / std::format("compare_{:04}{:02}{:02}_{:02}{:02}{:02}_{:06}.png", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, swap);
    if (WritePng(path, SideBySide(left, native))) REXLOG_INFO("[compare] wrote {} (Xenia {}x{} left, native {}x{} right)", path.string(), left.width, left.height, native.width, native.height);
    else REXLOG_WARN("[compare] could not write {}", path.string());
  }).detach();
}

}  // namespace cherry::compare
