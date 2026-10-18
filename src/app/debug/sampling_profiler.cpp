#include "app/debug/sampling_profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/frame_rate.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

REXCVAR_DEFINE_INT32(cherry_debug_profile_frame, 0, "Lollipop/Debug", "Presented frame at which to start sampling every thread for the profile log; 0 is off.");
REXCVAR_DEFINE_INT32(cherry_debug_profile_seconds, 10, "Lollipop/Debug", "How long the sampling profiler runs once started.").range(1, 120);

namespace cherry::app::debug {

namespace {

std::atomic<bool> g_stop{false};
std::thread g_thread;

#ifdef _WIN32
constexpr int kMaxDepth = 32;
constexpr int kTopFunctions = 25;

struct ThreadProfile {
  uint64_t samples = 0;
  std::unordered_map<uint64_t, uint64_t> self;
  std::unordered_map<uint64_t, uint64_t> inclusive;
};

// The start of the function holding `address`, so samples aggregate per function before any symbol lookup; leaf functions without unwind data keep the address.
uint64_t FunctionStart(uint64_t address) {
  DWORD64 image_base = 0;
  if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(address, &image_base, nullptr)) return image_base + function->BeginAddress;
  return address;
}

// Walks a suspended thread's stack; a frame caught mid-prologue can point the walk at garbage, so a fault just ends the walk.
int Unwind(CONTEXT context, uint64_t* frames) {
  int depth = 0;
  __try {
    for (; depth < kMaxDepth && context.Rip; ++depth) {
      frames[depth] = context.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {
        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
        context.Rsp += 8;
        continue;
      }
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return depth;
}

void Sample(HANDLE thread, ThreadProfile& profile) {
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(thread) == DWORD(-1)) return;
  uint64_t frames[kMaxDepth];
  int depth = 0;
  if (GetThreadContext(thread, &context)) depth = Unwind(context, frames);
  ResumeThread(thread);
  if (!depth) return;
  ++profile.samples;
  ++profile.self[FunctionStart(frames[0])];
  std::unordered_set<uint64_t> seen;
  for (int i = 0; i < depth; ++i) {
    const uint64_t start = FunctionStart(frames[i]);
    if (seen.insert(start).second) ++profile.inclusive[start];
  }
}

std::string Name(uint64_t address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) return symbol->Name;
  return std::format("{:016X}", address);
}

std::string ThreadName(DWORD id) {
  std::string name = std::to_string(id);
  if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id)) {
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description && *description) {
      name += " \"";
      for (const wchar_t* c = description; *c; ++c) name += *c < 128 ? char(*c) : '?';
      name += "\"";
    }
    if (description) LocalFree(description);
    CloseHandle(thread);
  }
  return name;
}

void Report(const std::map<DWORD, ThreadProfile>& profiles, uint64_t ticks) {
  std::vector<std::pair<DWORD, const ThreadProfile*>> busy;
  for (const auto& [id, profile] : profiles) {
    // A thread whose hottest function is a kernel wait is idle; only working threads are reported.
    uint64_t waiting = 0;
    for (const auto& [address, count] : profile.self) {
      const std::string name = Name(address);
      if (name.starts_with("Nt") || name.starts_with("Zw") || name.find("Wait") != std::string::npos || name.find("Sleep") != std::string::npos) waiting += count;
    }
    if (profile.samples && profile.samples - waiting > profile.samples / 20) busy.emplace_back(id, &profile);
  }
  std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.second->samples > b.second->samples; });
  REXLOG_INFO("[profile] {} sample ticks, {} busy threads", ticks, busy.size());
  for (const auto& [id, profile] : busy) {
    REXLOG_INFO("[profile] thread {}: {} samples", ThreadName(id), profile->samples);
    for (const auto* table : {&profile->self, &profile->inclusive}) {
      std::vector<std::pair<uint64_t, uint64_t>> top(table->begin(), table->end());
      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      top.resize(std::min<size_t>(top.size(), kTopFunctions));
      std::string lines;
      for (const auto& [address, count] : top) lines += std::format("\n    {:5.1f}% {}", 100.0 * double(count) / double(profile->samples), Name(address));
      REXLOG_INFO("[profile]   {}:{}", table == &profile->self ? "self" : "inclusive", lines);
    }
  }
}

void Run() {
  const uint64_t start_frame = uint64_t(REXCVAR_GET(cherry_debug_profile_frame));
  while (!g_stop.load() && cherry::engine::PresentedFrames() < start_frame) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (g_stop.load()) return;
  REXLOG_INFO("[profile] sampling every thread for {} s from frame {}", REXCVAR_GET(cherry_debug_profile_seconds), cherry::engine::PresentedFrames());
  std::map<DWORD, ThreadProfile> profiles;
  std::map<DWORD, HANDLE> handles;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(REXCVAR_GET(cherry_debug_profile_seconds));
  uint64_t ticks = 0;
  auto next_snapshot = std::chrono::steady_clock::now();
  while (!g_stop.load() && std::chrono::steady_clock::now() < end) {
    if (std::chrono::steady_clock::now() >= next_snapshot) {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry = {sizeof(entry)};
      for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId() || handles.count(entry.th32ThreadID)) continue;
        if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID)) handles[entry.th32ThreadID] = thread;
      }
      CloseHandle(snapshot);
      next_snapshot += std::chrono::seconds(1);
    }
    for (const auto& [id, thread] : handles) Sample(thread, profiles[id]);
    ++ticks;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (const auto& [id, thread] : handles) CloseHandle(thread);
  Report(profiles, ticks);
}
#endif

}  // namespace

void StartSamplingProfiler() {
#ifdef _WIN32
  if (REXCVAR_GET(cherry_debug_profile_frame) <= 0 || g_thread.joinable()) return;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  g_thread = std::thread(Run);
#endif
}

void StopSamplingProfiler() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace cherry::app::debug
