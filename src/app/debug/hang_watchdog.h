#pragma once

namespace cherry::app {

// With --cherry_debug_hang_dump, logs every thread's host stack once when no frame has been presented for 8 seconds.
void StartHangWatchdog();
void StopHangWatchdog();

}  // namespace cherry::app
