// Paired screenshots for the comparison mode: Xenia's image and the native renderer's for the same guest swap, side by side in one PNG.
#pragma once

#include <functional>

namespace cherry::compare {

// Captures the next swap; bind_compare_capture (F8) in the game window and F8 in the native window call it.
void RequestPairedCapture();
void RegisterCaptureBind();
// From the Swap hook before the frame goes to the native renderer: true when this swap is captured, with the native side armed.
bool BeginPairedCapture();
// After the frame went to the native renderer: `wait_for_xenia` returns once Xenia has drawn the swap, then both images are written.
void FinishPairedCapture(const std::function<void()>& wait_for_xenia);

}  // namespace cherry::compare
