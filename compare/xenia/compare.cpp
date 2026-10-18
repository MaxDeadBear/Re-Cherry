#include "compare/xenia/compare.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <windows.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>

#include "compare/xenia/paired_capture.h"
#include "gpu/graphics_system.h"

namespace cherry::compare {

namespace {

std::unique_ptr<gpu::GraphicsSystem> g_native;
std::thread g_window_thread;
std::atomic<HWND> g_window{nullptr};
std::atomic<DWORD> g_window_thread_id{0};
std::mutex g_window_mutex;
std::condition_variable g_window_ready;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  // Closing the comparison window only hides it; the game keeps running in Xenia's window.
  if (message == WM_CLOSE) {
    ShowWindow(window, SW_HIDE);
    return 0;
  }
  if (message == WM_KEYDOWN && wparam == VK_F8) {
    RequestPairedCapture();
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

// The native renderer's window lives on its own thread with its own message loop, so it never depends on the runtime's window.
void WindowThread() {
  g_window_thread_id = GetCurrentThreadId();
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class = {};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"ReCherryNativeCompare";
  RegisterClassW(&window_class);
  RECT rect = {0, 0, 1280, 720};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Re-Cherry native renderer (compare)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  {
    std::lock_guard<std::mutex> lock(g_window_mutex);
    g_window = window;
  }
  g_window_ready.notify_all();
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

}  // namespace

void ConfigureRuntime(rex::RuntimeConfig& config) {
  config.graphics.reset();
  config.gpu_plugin = "xenos";
  g_window_thread = std::thread(WindowThread);
  {
    std::unique_lock<std::mutex> lock(g_window_mutex);
    g_window_ready.wait(lock, [] { return g_window.load() != nullptr; });
  }
  g_native = std::make_unique<gpu::GraphicsSystem>([] { return static_cast<void*>(g_window.load()); }, gpu::ConfiguredBackend());
  if (g_native->SetupPresentation(nullptr) != 0) REXLOG_ERROR("[compare] the native renderer could not start; only Xenia's window will draw");
  REXLOG_INFO("[compare] Xenia GPU plugin in the main window, native renderer in its own window");
}

void ArrangeWindows(rex::ui::Window* game_window) {
  if (!game_window) return;
  RegisterCaptureBind();
  game_window->SetFullscreen(false);
  // Leaving fullscreen lands on the window's next event loop tick, so the placement waits for it.
  std::thread([game_window] {
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    RECT area = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    const int half = (area.right - area.left) / 2;
    const int height = area.bottom - area.top;
    if (HWND game = static_cast<HWND>(game_window->GetNativeWindowHandle())) SetWindowPos(game, nullptr, area.left, area.top, half, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (HWND native = g_window.load()) SetWindowPos(native, nullptr, area.left + half, area.top, half, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }).detach();
}

void Shutdown() {
  if (g_native) g_native->Shutdown();
  g_native.reset();
  g_window = nullptr;
  if (const DWORD thread = g_window_thread_id.exchange(0)) PostThreadMessageW(thread, WM_QUIT, 0, 0);
  if (g_window_thread.joinable()) g_window_thread.join();
}

}  // namespace cherry::compare
