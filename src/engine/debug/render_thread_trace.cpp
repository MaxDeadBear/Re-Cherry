// Logs the UE3 rendering thread's lifecycle under --cherry_debug_hang_dump, to see where it stops during renderer bring-up.

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, cherry_debug_hang_dump);

namespace {

bool Tracing() {
  return REXCVAR_GET(cherry_debug_hang_dump);
}

}  // namespace

// StartRenderingThread()
REX_HOOK_RAW(sub_823AC188) {
  if (Tracing()) REXLOG_WARN("[rt] StartRenderingThread");
  __imp__sub_823AC188(ctx, base);
  if (Tracing()) REXLOG_WARN("[rt] StartRenderingThread returned");
}

// StopRenderingThread()
REX_HOOK_RAW(sub_823AC2A0) {
  if (Tracing()) REXLOG_WARN("[rt] StopRenderingThread");
  __imp__sub_823AC2A0(ctx, base);
}

// RenderingThreadMain()
REX_HOOK_RAW(sub_823ABD38) {
  if (Tracing()) REXLOG_WARN("[rt] RenderingThreadMain entered");
  __imp__sub_823ABD38(ctx, base);
  if (Tracing()) REXLOG_WARN("[rt] RenderingThreadMain returned");
}

// FRenderingThread::Run's caller-side entry, slot 1 of its FRunnable vtable.
REX_HOOK_RAW(sub_823AC160) {
  if (Tracing()) REXLOG_WARN("[rt] FRenderingThread::Run");
  __imp__sub_823AC160(ctx, base);
  if (Tracing()) REXLOG_WARN("[rt] FRenderingThread::Run returned {}", ctx.r3.u32);
}
