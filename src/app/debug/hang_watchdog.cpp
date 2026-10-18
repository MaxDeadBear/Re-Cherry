#include "app/debug/hang_watchdog.h"

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/frame_rate.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

REXCVAR_DEFINE_BOOL(cherry_debug_hang_dump, false, "Lollipop/Debug", "Log hardware exceptions the game swallows, and every thread's host stack once if no frame is presented for 8 seconds.");

namespace cherry::app {

namespace {

std::atomic<bool> g_stop{false};
std::thread g_thread;

#ifdef _WIN32
std::string Describe(HANDLE process, DWORD64 address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol)) return std::string(symbol->Name) + "+0x" + std::format("{:X}", displacement);
  return std::format("{:016X}", address);
}

std::string UnwindTrace(HANDLE process, CONTEXT context) {
  std::string trace;
  for (int frame = 0; frame < 12 && context.Rip; ++frame) {
    trace += "\n    " + Describe(process, context.Rip);
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
  return trace;
}

void* g_exception_handler = nullptr;

// Registered last, so the SDK's MMIO handler has already taken its own faults; the game's __try blocks swallow the rest silently.
LONG CALLBACK LogGuestException(EXCEPTION_POINTERS* info) {
  static std::atomic<int> logged{0};
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  constexpr DWORD kCppException = 0xE06D7363;
  constexpr DWORD kSetThreadName = 0x406D1388;
  if (code == kCppException || code == kSetThreadName || code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C || logged.fetch_add(1) >= 16) return EXCEPTION_CONTINUE_SEARCH;
  const ULONG_PTR* params = info->ExceptionRecord->ExceptionInformation;
  const ULONG_PTR address = info->ExceptionRecord->NumberParameters >= 2 ? params[1] : 0;
  REXLOG_WARN("[exception] thread {} code {:08X} at address {:016X}:{}", GetCurrentThreadId(), code, address, UnwindTrace(GetCurrentProcess(), *info->ContextRecord));
  return EXCEPTION_CONTINUE_SEARCH;
}

void DumpThreads() {
  HANDLE process = GetCurrentProcess();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry = {sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    SuspendThread(thread);
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    std::string trace;
    if (GetThreadContext(thread, &context)) trace = UnwindTrace(process, context);
    ResumeThread(thread);
    CloseHandle(thread);
    REXLOG_WARN("[hang] thread {}:{}", entry.th32ThreadID, trace);
  }
  CloseHandle(snapshot);
}
#endif

}  // namespace

void StartHangWatchdog() {
#ifdef _WIN32
  if (!REXCVAR_GET(cherry_debug_hang_dump) || g_thread.joinable()) return;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  g_exception_handler = AddVectoredExceptionHandler(0, LogGuestException);
  REXLOG_WARN("[hang] watchdog and exception logging armed");
  g_thread = std::thread([] {
    bool dumped = false;
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const double idle = cherry::engine::SecondsSinceLastPresent();
      if (!dumped && idle > 8.0) {
        REXLOG_WARN("[hang] no frame presented for {:.1f} s; dumping thread stacks", idle);
        DumpThreads();
        dumped = true;
      }
    }
  });
#endif
}

void StopHangWatchdog() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
#ifdef _WIN32
  if (g_exception_handler) RemoveVectoredExceptionHandler(g_exception_handler);
  g_exception_handler = nullptr;
#endif
}

}  // namespace cherry::app
